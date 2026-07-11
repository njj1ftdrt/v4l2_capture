#!/usr/bin/env bash
set -euo pipefail

DEVICE="${DEVICE:-/dev/video0}"
WIDTH="${WIDTH:-640}"
HEIGHT="${HEIGHT:-360}"
FORMAT="${FORMAT:-YUYV}"
FRAMES="${FRAMES:-300}"
RING_CAPACITY="${RING_CAPACITY:-8}"
TCP_QUEUE_CAPACITY="${TCP_QUEUE_CAPACITY:-8}"
PORT="${PORT:-9000}"
TIMEOUT_MS="${TIMEOUT_MS:-2000}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${ROOT_DIR}/docs/logs"
OUTPUT_DIR="${ROOT_DIR}/output/tcp_regression_${FRAMES}"

RECEIVER_LOG="${LOG_DIR}/tcp_regression_${FRAMES}_receiver.txt"
SENDER_LOG="${LOG_DIR}/tcp_regression_${FRAMES}_sender.txt"
EXPECTED_SIZE=$((WIDTH * HEIGHT * 2))

mkdir -p "${LOG_DIR}"
rm -rf "${OUTPUT_DIR}"

cleanup() {
    if [[ -n "${RECEIVER_PID:-}" ]] && kill -0 "${RECEIVER_PID}" 2>/dev/null; then
        kill "${RECEIVER_PID}" 2>/dev/null || true
        wait "${RECEIVER_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

extract_metric() {
    local label="$1"
    local file="$2"

    awk -F: -v label="${label}" '
        index($0, label) {
            value = $NF
            gsub(/[[:space:]]/, "", value)
            last = value
        }
        END {
            if (last == "") {
                exit 1
            }
            print last
        }
    ' "${file}"
}

echo "========== TCP V4L2 Regression Test =========="
echo "device             : ${DEVICE}"
echo "resolution         : ${WIDTH}x${HEIGHT}"
echo "format             : ${FORMAT}"
echo "frames             : ${FRAMES}"
echo "ring capacity      : ${RING_CAPACITY}"
echo "tcp queue capacity : ${TCP_QUEUE_CAPACITY}"
echo "port               : ${PORT}"
echo "expected size      : ${EXPECTED_SIZE}"
echo "output             : ${OUTPUT_DIR}"
echo "=============================================="

# Keep receiver output in its own log to avoid interleaving two processes on the terminal.
"${BUILD_DIR}/tcp_receiver" \
  --port "${PORT}" \
  --output "${OUTPUT_DIR}" \
  --max-frames "${FRAMES}" \
  >"${RECEIVER_LOG}" 2>&1 &
RECEIVER_PID=$!

sleep 1

set +e
"${BUILD_DIR}/v4l2_capture" \
  --config "${ROOT_DIR}/config/v4l2_tcp_pipeline.conf" \
  --device "${DEVICE}" \
  --width "${WIDTH}" \
  --height "${HEIGHT}" \
  --format "${FORMAT}" \
  --mmap-buffers 4 \
  --pipeline-frames "${FRAMES}" \
  --ring-capacity "${RING_CAPACITY}" \
  --tcp-host 127.0.0.1 \
  --tcp-port "${PORT}" \
  --tcp-queue-capacity "${TCP_QUEUE_CAPACITY}" \
  --timeout-ms "${TIMEOUT_MS}" \
  2>&1 | tee "${SENDER_LOG}"
SENDER_STATUS=${PIPESTATUS[0]}
set -e

wait "${RECEIVER_PID}" || true
RECEIVER_PID=""

if [[ "${SENDER_STATUS}" -ne 0 ]]; then
    echo "[FAIL] sender exited with status ${SENDER_STATUS}"
    exit 1
fi

PRODUCED_FRAMES="$(extract_metric "produced frames" "${SENDER_LOG}")"
CONSUMED_FRAMES="$(extract_metric "consumed frames" "${SENDER_LOG}")"
TCP_SENT_FRAMES="$(extract_metric "tcp sent frames" "${SENDER_LOG}")"
TCP_SEND_ERRORS="$(extract_metric "tcp send errors" "${SENDER_LOG}")"
INVALID_FRAMES="$(extract_metric "invalid frames" "${SENDER_LOG}")"
RING_DROPPED="$(extract_metric "ring dropped frames" "${SENDER_LOG}")"
TCP_QUEUE_DROPPED="$(extract_metric "tcp queue dropped" "${SENDER_LOG}")"
RECEIVED_FRAMES="$(extract_metric "received frames" "${RECEIVER_LOG}")"
CRC_ERRORS="$(extract_metric "crc errors" "${RECEIVER_LOG}")"

FILE_COUNT="$(find "${OUTPUT_DIR}" -type f -name '*.YUYV' | wc -l | tr -d '[:space:]')"
BAD_SIZE_COUNT="$(
    find "${OUTPUT_DIR}" -type f -name '*.YUYV' -printf '%s\n' \
    | awk -v expected="${EXPECTED_SIZE}" '$1 != expected {count++} END {print count + 0}'
)"

VALID_ACCOUNTED=$((TCP_SENT_FRAMES + INVALID_FRAMES))

echo "========== Verification =========="
echo "produced frames      : ${PRODUCED_FRAMES}"
echo "consumed frames      : ${CONSUMED_FRAMES}"
echo "tcp sent frames      : ${TCP_SENT_FRAMES}"
echo "tcp send errors      : ${TCP_SEND_ERRORS}"
echo "invalid frames       : ${INVALID_FRAMES}"
echo "ring dropped frames  : ${RING_DROPPED}"
echo "tcp queue dropped    : ${TCP_QUEUE_DROPPED}"
echo "receiver frames      : ${RECEIVED_FRAMES}"
echo "crc errors           : ${CRC_ERRORS}"
echo "received files       : ${FILE_COUNT}"
echo "bad size files       : ${BAD_SIZE_COUNT}"
echo "expected file size   : ${EXPECTED_SIZE}"
echo "=================================="

if [[ "${PRODUCED_FRAMES}" != "${CONSUMED_FRAMES}" ]]; then
    echo "[FAIL] produced and consumed frame counts differ"
    exit 1
fi

if [[ "${TCP_SEND_ERRORS}" != "0" ]]; then
    echo "[FAIL] tcp send errors detected"
    exit 1
fi

if [[ "${RING_DROPPED}" != "0" ]]; then
    echo "[FAIL] main RingBuffer dropped frames"
    exit 1
fi

if [[ "${TCP_QUEUE_DROPPED}" != "0" ]]; then
    echo "[FAIL] TCP RingBuffer dropped frames"
    exit 1
fi

if [[ "${CRC_ERRORS}" != "0" ]]; then
    echo "[FAIL] receiver detected CRC errors"
    exit 1
fi

if [[ "${FILE_COUNT}" != "${TCP_SENT_FRAMES}" ]]; then
    echo "[FAIL] received file count does not match tcp sent frames"
    exit 1
fi

if [[ "${RECEIVED_FRAMES}" != "${TCP_SENT_FRAMES}" ]]; then
    echo "[FAIL] receiver frame count does not match tcp sent frames"
    exit 1
fi

if [[ "${BAD_SIZE_COUNT}" != "0" ]]; then
    echo "[FAIL] some received YUYV files have unexpected size"
    exit 1
fi

if [[ "${VALID_ACCOUNTED}" != "${CONSUMED_FRAMES}" ]]; then
    echo "[FAIL] sent + invalid frames do not account for all consumed frames"
    exit 1
fi

if [[ "${INVALID_FRAMES}" != "0" ]]; then
    echo "[WARN] camera produced ${INVALID_FRAMES} invalid frame(s); valid transmitted frames still passed end-to-end verification"
fi

echo "[PASS] TCP V4L2 regression test passed."
