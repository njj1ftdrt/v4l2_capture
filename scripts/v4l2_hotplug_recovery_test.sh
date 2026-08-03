#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
DEVICE="${DEVICE:-/dev/video0}"
FRAMES="${FRAMES:-10000}"
PORT="${PORT:-19000}"
TIMEOUT_MS="${TIMEOUT_MS:-1000}"
TIMEOUT_THRESHOLD="${TIMEOUT_THRESHOLD:-3}"
INVALID_FRAME_THRESHOLD="${INVALID_FRAME_THRESHOLD:-5}"
RECOVERY_ATTEMPTS="${RECOVERY_ATTEMPTS:-30}"
RECOVERY_DELAY_MS="${RECOVERY_DELAY_MS:-1000}"
LOG_DIR="${LOG_DIR:-${ROOT_DIR}/output/hotplug_recovery}"
STATS_FILE="${LOG_DIR}/pipeline_stats.json"

mkdir -p "${LOG_DIR}"

CAPTURE_BIN="${BUILD_DIR}/v4l2_capture"
RECEIVER_BIN="${BUILD_DIR}/tcp_receiver"

if [[ ! -x "${CAPTURE_BIN}" || ! -x "${RECEIVER_BIN}" ]]; then
    echo "[ERROR] Build binaries first: cmake -S . -B ${BUILD_DIR} && cmake --build ${BUILD_DIR} -j" >&2
    exit 2
fi

if [[ ! -e "${DEVICE}" ]]; then
    echo "[ERROR] Camera device does not exist: ${DEVICE}" >&2
    exit 2
fi

cleanup() {
    set +e
    [[ -n "${CAPTURE_PID:-}" ]] && kill -INT "${CAPTURE_PID}" 2>/dev/null
    [[ -n "${RECEIVER_PID:-}" ]] && kill -INT "${RECEIVER_PID}" 2>/dev/null
    wait "${CAPTURE_PID:-}" 2>/dev/null
    wait "${RECEIVER_PID:-}" 2>/dev/null
}
trap cleanup EXIT INT TERM

"${RECEIVER_BIN}" \
    --port "${PORT}" \
    --output "${LOG_DIR}/received" \
    --max-frames "${FRAMES}" \
    --stats-output "${LOG_DIR}/receiver_stats.json" \
    >"${LOG_DIR}/receiver.log" 2>&1 &
RECEIVER_PID=$!

sleep 0.5

"${CAPTURE_BIN}" \
    --device "${DEVICE}" \
    --width 640 \
    --height 360 \
    --format YUYV \
    --mmap-buffers 4 \
    --pipeline-frames "${FRAMES}" \
    --ring-capacity 8 \
    --tcp-host 127.0.0.1 \
    --tcp-port "${PORT}" \
    --tcp-queue-capacity 8 \
    --tcp-send-timeout-ms 2000 \
    --timeout-ms "${TIMEOUT_MS}" \
    --camera-timeout-recovery-threshold "${TIMEOUT_THRESHOLD}" \
    --camera-invalid-frame-recovery-threshold "${INVALID_FRAME_THRESHOLD}" \
    --camera-recovery-max-attempts "${RECOVERY_ATTEMPTS}" \
    --camera-recovery-retry-delay-ms "${RECOVERY_DELAY_MS}" \
    --stats-output "${STATS_FILE}" \
    >"${LOG_DIR}/camera.log" 2>&1 &
CAPTURE_PID=$!

cat <<INSTRUCTIONS
[TEST] Capture is running with PID ${CAPTURE_PID}.
1. Confirm frames are arriving: tail -f ${LOG_DIR}/camera.log
2. Unplug or detach the USB camera from the VM.
3. Wait until CAMERA_RECOVERY attempts appear in the log.
4. Reattach the same camera and ensure ${DEVICE} returns.
5. The program should rebuild the stream and continue producing frames.
Press Enter after the camera has recovered, or Ctrl-C to abort.
INSTRUCTIONS
read -r

sleep 2
kill -INT "${CAPTURE_PID}" 2>/dev/null || true
wait "${CAPTURE_PID}" || CAPTURE_RC=$?
CAPTURE_RC="${CAPTURE_RC:-0}"

kill -INT "${RECEIVER_PID}" 2>/dev/null || true
wait "${RECEIVER_PID}" || true

if [[ ! -f "${STATS_FILE}" ]]; then
    echo "[FAIL] Stats file was not produced: ${STATS_FILE}" >&2
    exit 1
fi

python3 - "${STATS_FILE}" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    stats = json.load(f)

attempts = int(stats.get("camera_recovery_attempts", 0))
successes = int(stats.get("camera_recovery_successes", 0))
failures = int(stats.get("camera_recovery_failures", 0))
buffer_errors = int(stats.get("camera_buffer_error_frames", 0))
incomplete = int(stats.get("camera_incomplete_frames", 0))
invalid_peak = int(stats.get("camera_consecutive_invalid_peak", 0))
invalid_recoveries = int(stats.get("camera_invalid_frame_recoveries", 0))
produced = int(stats.get("produced_frames", 0))

print(f"[RESULT] produced_frames={produced}")
print(f"[RESULT] camera_recovery_attempts={attempts}")
print(f"[RESULT] camera_recovery_successes={successes}")
print(f"[RESULT] camera_recovery_failures={failures}")
print(f"[RESULT] camera_buffer_error_frames={buffer_errors}")
print(f"[RESULT] camera_incomplete_frames={incomplete}")
print(f"[RESULT] camera_consecutive_invalid_peak={invalid_peak}")
print(f"[RESULT] camera_invalid_frame_recoveries={invalid_recoveries}")

if attempts < 1:
    raise SystemExit("[FAIL] No recovery attempt was observed")
if successes < 1:
    raise SystemExit("[FAIL] No successful camera recovery was observed")
if produced < 2:
    raise SystemExit("[FAIL] Too few frames were produced to prove post-recovery capture")

print("[PASS] At least one camera recovery succeeded; inspect logs for pre/post-recovery frame counts")
PY

if [[ "${CAPTURE_RC}" -ne 0 && "${CAPTURE_RC}" -ne 130 ]]; then
    echo "[WARN] Capture process exit code: ${CAPTURE_RC}; inspect ${LOG_DIR}/camera.log"
fi
