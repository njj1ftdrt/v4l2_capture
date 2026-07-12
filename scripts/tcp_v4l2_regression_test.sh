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
TCP_CONNECT_MAX_ATTEMPTS="${TCP_CONNECT_MAX_ATTEMPTS:-5}"
TCP_CONNECT_RETRY_DELAY_MS="${TCP_CONNECT_RETRY_DELAY_MS:-500}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${ROOT_DIR}/docs/logs"
OUTPUT_DIR="${ROOT_DIR}/output/tcp_regression_${FRAMES}"
STATS_DIR="${ROOT_DIR}/output/stats"
SENDER_STATS_JSON="${STATS_DIR}/pipeline_stats.json"
RECEIVER_STATS_JSON="${STATS_DIR}/receiver_stats.json"

RECEIVER_LOG="${LOG_DIR}/tcp_regression_${FRAMES}_receiver.txt"
SENDER_LOG="${LOG_DIR}/tcp_regression_${FRAMES}_sender.txt"
EXPECTED_SIZE=$((WIDTH * HEIGHT * 2))

mkdir -p "${LOG_DIR}"
rm -rf "${OUTPUT_DIR}"
rm -rf "${STATS_DIR}"
mkdir -p "${STATS_DIR}"

cleanup() {
    if [[ -n "${RECEIVER_PID:-}" ]] && kill -0 "${RECEIVER_PID}" 2>/dev/null; then
        kill "${RECEIVER_PID}" 2>/dev/null || true
        wait "${RECEIVER_PID}" 2>/dev/null || true
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
echo "connect attempts   : ${TCP_CONNECT_MAX_ATTEMPTS}"
echo "retry delay ms     : ${TCP_CONNECT_RETRY_DELAY_MS}"
echo "expected size      : ${EXPECTED_SIZE}"
echo "output             : ${OUTPUT_DIR}"
echo "sender stats json  : ${SENDER_STATS_JSON}"
echo "receiver stats json: ${RECEIVER_STATS_JSON}"
echo "=============================================="

"${BUILD_DIR}/tcp_receiver" \
  --port "${PORT}" \
  --output "${OUTPUT_DIR}" \
  --max-frames "${FRAMES}" \
  --stats-output "${RECEIVER_STATS_JSON}" \
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
  --tcp-connect-max-attempts "${TCP_CONNECT_MAX_ATTEMPTS}" \
  --tcp-connect-retry-delay-ms "${TCP_CONNECT_RETRY_DELAY_MS}" \
  --timeout-ms "${TIMEOUT_MS}" \
  --stats-output "${SENDER_STATS_JSON}" \
  2>&1 | tee "${SENDER_LOG}"
SENDER_STATUS=${PIPESTATUS[0]}
set -e

wait "${RECEIVER_PID}" || true
RECEIVER_PID=""

if [[ "${SENDER_STATUS}" -ne 0 ]]; then
    echo "[FAIL] sender exited with status ${SENDER_STATUS}"
    exit 1
fi

if [[ ! -f "${SENDER_STATS_JSON}" ]]; then
    echo "[FAIL] sender stats json not found: ${SENDER_STATS_JSON}"
    exit 1
fi

if [[ ! -f "${RECEIVER_STATS_JSON}" ]]; then
    echo "[FAIL] receiver stats json not found: ${RECEIVER_STATS_JSON}"
    exit 1
fi

FILE_COUNT="$(find "${OUTPUT_DIR}" -type f -name '*.YUYV' | wc -l | tr -d '[:space:]')"
BAD_SIZE_COUNT="$(
    find "${OUTPUT_DIR}" -type f -name '*.YUYV' -printf '%s\n' \
    | awk -v expected="${EXPECTED_SIZE}" '$1 != expected {count++} END {print count + 0}'
)"

export SENDER_STATS_JSON RECEIVER_STATS_JSON FILE_COUNT BAD_SIZE_COUNT EXPECTED_SIZE
JSON_VALUES="$(python3 - <<'PY'
import json
import os

with open(os.environ['SENDER_STATS_JSON'], encoding='utf-8') as f:
    sender = json.load(f)
with open(os.environ['RECEIVER_STATS_JSON'], encoding='utf-8') as f:
    receiver = json.load(f)

pairs = {
    'PRODUCED_FRAMES': sender['produced_frames'],
    'CONSUMED_FRAMES': sender['consumed_frames'],
    'TCP_SENT_FRAMES': sender['tcp_sent_frames'],
    'TCP_SEND_ERRORS': sender['tcp_send_errors'],
    'TCP_CONNECT_ATTEMPTS': sender['tcp_connect_attempts'],
    'TCP_CONNECT_RETRIES': sender['tcp_connect_retries'],
    'INVALID_FRAMES': sender['invalid_frames'],
    'RING_DROPPED': sender['ring_dropped_frames'],
    'TCP_QUEUE_DROPPED': sender['tcp_queue_dropped'],
    'RECEIVER_FRAMES': receiver['received_frames'],
    'CRC_ERRORS': receiver['crc_errors'],
    'HEADER_ERRORS': receiver['header_errors'],
    'REJECTED_FRAMES': receiver['rejected_frames'],
    'RECEIVED_FILES': int(os.environ['FILE_COUNT']),
    'BAD_SIZE_FILES': int(os.environ['BAD_SIZE_COUNT']),
    'EXPECTED_SIZE': int(os.environ['EXPECTED_SIZE']),
}

for key, value in pairs.items():
    print(f'{key}={value}')
PY
)"
eval "${JSON_VALUES}"

VALID_ACCOUNTED=$((TCP_SENT_FRAMES + INVALID_FRAMES))

echo "========== Verification =========="
echo "produced frames      : ${PRODUCED_FRAMES}"
echo "consumed frames      : ${CONSUMED_FRAMES}"
echo "tcp sent frames      : ${TCP_SENT_FRAMES}"
echo "tcp send errors      : ${TCP_SEND_ERRORS}"
echo "tcp connect attempts : ${TCP_CONNECT_ATTEMPTS}"
echo "tcp connect retries  : ${TCP_CONNECT_RETRIES}"
echo "invalid frames       : ${INVALID_FRAMES}"
echo "ring dropped frames  : ${RING_DROPPED}"
echo "tcp queue dropped    : ${TCP_QUEUE_DROPPED}"
echo "receiver frames      : ${RECEIVER_FRAMES}"
echo "crc errors           : ${CRC_ERRORS}"
echo "header errors        : ${HEADER_ERRORS}"
echo "rejected frames      : ${REJECTED_FRAMES}"
echo "received files       : ${RECEIVED_FILES}"
echo "bad size files       : ${BAD_SIZE_FILES}"
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

if [[ "${TCP_CONNECT_ATTEMPTS}" != "1" ]]; then
    echo "[FAIL] receiver was already listening, so the sender should connect on the first attempt"
    exit 1
fi

if [[ "${TCP_CONNECT_RETRIES}" != "0" ]]; then
    echo "[FAIL] unexpected TCP connection retries in normal regression"
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

if [[ "${HEADER_ERRORS}" != "0" ]]; then
    echo "[FAIL] receiver detected invalid protocol headers"
    exit 1
fi

if [[ "${REJECTED_FRAMES}" != "0" ]]; then
    echo "[FAIL] receiver rejected frames during normal regression"
    exit 1
fi

if [[ "${RECEIVED_FILES}" != "${TCP_SENT_FRAMES}" ]]; then
    echo "[FAIL] received file count does not match tcp sent frames"
    exit 1
fi

if [[ "${RECEIVER_FRAMES}" != "${TCP_SENT_FRAMES}" ]]; then
    echo "[FAIL] receiver frame count does not match tcp sent frames"
    exit 1
fi

if [[ "${BAD_SIZE_FILES}" != "0" ]]; then
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
