#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
DEVICE="${DEVICE:-/dev/video0}"
FRAMES="${FRAMES:-30000}"
PORT="${PORT:-19100}"
INVALID_FRAME_THRESHOLD="${INVALID_FRAME_THRESHOLD:-30}"
INVALID_RECOVERY_COOLDOWN_FRAMES="${INVALID_RECOVERY_COOLDOWN_FRAMES:-300}"
INVALID_RECOVERY_MAX_COUNT="${INVALID_RECOVERY_MAX_COUNT:-3}"
LOG_DIR="${LOG_DIR:-${ROOT_DIR}/output/frame_quality}"
CAMERA_STATS="${LOG_DIR}/pipeline_stats.json"
RECEIVER_STATS="${LOG_DIR}/receiver_stats.json"

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

rm -rf "${LOG_DIR}"
mkdir -p "${LOG_DIR}"

cleanup() {
    set +e
    [[ -n "${CAPTURE_PID:-}" ]] && kill -INT "${CAPTURE_PID}" 2>/dev/null
    [[ -n "${RECEIVER_PID:-}" ]] && kill -INT "${RECEIVER_PID}" 2>/dev/null
    [[ -n "${CAPTURE_PID:-}" ]] && wait "${CAPTURE_PID}" 2>/dev/null
    [[ -n "${RECEIVER_PID:-}" ]] && wait "${RECEIVER_PID}" 2>/dev/null
}
trap cleanup EXIT INT TERM

"${RECEIVER_BIN}" \
    --port "${PORT}" \
    --output "${LOG_DIR}/received" \
    --max-frames "${FRAMES}" \
    --stats-output "${RECEIVER_STATS}" \
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
    --timeout-ms 1000 \
    --camera-timeout-recovery-threshold 3 \
    --camera-invalid-frame-recovery-threshold "${INVALID_FRAME_THRESHOLD}" \
    --camera-invalid-frame-recovery-cooldown-frames "${INVALID_RECOVERY_COOLDOWN_FRAMES}" \
    --camera-invalid-frame-recovery-max-count "${INVALID_RECOVERY_MAX_COUNT}" \
    --camera-recovery-max-attempts 5 \
    --camera-recovery-retry-delay-ms 1000 \
    --stats-output "${CAMERA_STATS}" \
    >"${LOG_DIR}/camera.log" 2>&1 &
CAPTURE_PID=$!

wait "${CAPTURE_PID}"
unset CAPTURE_PID

kill -INT "${RECEIVER_PID}" 2>/dev/null || true
wait "${RECEIVER_PID}" || true
unset RECEIVER_PID

python3 - "${CAMERA_STATS}" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    s = json.load(f)

produced = int(s.get("produced_frames", 0))
consumed = int(s.get("consumed_frames", 0))
dropped = int(s.get("ring_dropped_frames", 0))
invalid = int(s.get("invalid_frames", 0))
queued = int(s.get("tcp_queued_frames", 0))
sent = int(s.get("tcp_sent_frames", 0))
buffer_errors = int(s.get("camera_buffer_error_frames", 0))
incomplete = int(s.get("camera_incomplete_frames", 0))
peak = int(s.get("camera_consecutive_invalid_peak", 0))
invalid_recoveries = int(s.get("camera_invalid_frame_recoveries", 0))
suppressed_cooldown = int(s.get("camera_invalid_recovery_suppressed_cooldown", 0))
suppressed_budget = int(s.get("camera_invalid_recovery_suppressed_budget", 0))
send_errors = int(s.get("tcp_send_errors", 0))
send_timeouts = int(s.get("tcp_send_timeouts", 0))

print(f"[RESULT] produced={produced} consumed={consumed} ring_dropped={dropped}")
print(f"[RESULT] invalid={invalid} incomplete={incomplete} buffer_error={buffer_errors}")
print(f"[RESULT] invalid_streak_peak={peak} invalid_recoveries={invalid_recoveries}")
print(f"[RESULT] invalid_recovery_suppressed_cooldown={suppressed_cooldown} suppressed_budget={suppressed_budget}")
print(f"[RESULT] tcp_queued={queued} tcp_sent={sent}")
print(f"[RESULT] invalid_ratio={(100.0 * invalid / produced) if produced else 0.0:.3f}%")

if produced != consumed + dropped:
    raise SystemExit("[FAIL] produced != consumed + ring_dropped")
if consumed != invalid + queued:
    raise SystemExit("[FAIL] consumed != invalid + tcp_queued")
if queued != sent:
    raise SystemExit("[FAIL] tcp_queued != tcp_sent")
if send_errors != 0 or send_timeouts != 0:
    raise SystemExit("[FAIL] TCP send errors/timeouts were observed")

print("[PASS] Frame-quality accounting invariants hold")
PY

printf '[INFO] Logs: %s\n' "${LOG_DIR}"
