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
    if [[ -n "${RECEIVER_PID:-}" ]]; then
        if kill -0 "${RECEIVER_PID}" 2>/dev/null; then
            kill "${RECEIVER_PID}" 2>/dev/null || true
            wait "${RECEIVER_PID}" 2>/dev/null || true
        fi
    fi
}
trap cleanup EXIT

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

"${BUILD_DIR}/tcp_receiver" \
  --port "${PORT}" \
  --output "${OUTPUT_DIR}" \
  --max-frames "${FRAMES}" \
  2>&1 | tee "${RECEIVER_LOG}" &

RECEIVER_PID=$!

sleep 1

set +e
"${BUILD_DIR}/v4l2_capture" \
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

TCP_SENT_FRAMES="$(awk -F: '/tcp sent frames/ {gsub(/ /, "", $2); print $2}' "${SENDER_LOG}" | tail -1)"
TCP_SEND_ERRORS="$(awk -F: '/tcp send errors/ {gsub(/ /, "", $2); print $2}' "${SENDER_LOG}" | tail -1)"
INVALID_FRAMES="$(awk -F: '/invalid frames/ {gsub(/ /, "", $2); print $2}' "${SENDER_LOG}" | tail -1)"
RING_DROPPED="$(awk -F: '/ring dropped frames/ {gsub(/ /, "", $2); print $2}' "${SENDER_LOG}" | tail -1)"

FILE_COUNT="$(find "${OUTPUT_DIR}" -type f -name '*.YUYV' | wc -l)"

BAD_SIZE_COUNT="$(
    find "${OUTPUT_DIR}" -type f -name '*.YUYV' -printf '%s\n' \
    | awk -v expected="${EXPECTED_SIZE}" '$1 != expected {count++} END {print count + 0}'
)"

echo "========== Verification =========="
echo "tcp sent frames      : ${TCP_SENT_FRAMES}"
echo "tcp send errors      : ${TCP_SEND_ERRORS}"
echo "invalid frames       : ${INVALID_FRAMES}"
echo "ring dropped frames  : ${RING_DROPPED}"
echo "received files       : ${FILE_COUNT}"
echo "bad size files       : ${BAD_SIZE_COUNT}"
echo "expected file size   : ${EXPECTED_SIZE}"
echo "=================================="

if [[ "${TCP_SEND_ERRORS}" != "0" ]]; then
    echo "[FAIL] tcp send errors detected"
    exit 1
fi

if [[ "${RING_DROPPED}" != "0" ]]; then
    echo "[FAIL] ring dropped frames detected"
    exit 1
fi

if [[ "${FILE_COUNT}" != "${TCP_SENT_FRAMES}" ]]; then
    echo "[FAIL] received file count does not match tcp sent frames"
    exit 1
fi

if [[ "${BAD_SIZE_COUNT}" != "0" ]]; then
    echo "[FAIL] some received YUYV files have unexpected size"
    exit 1
fi

echo "[PASS] TCP V4L2 regression test passed."
