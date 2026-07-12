#!/usr/bin/env bash
set -euo pipefail

DEVICE="${DEVICE:-/dev/video0}"
WIDTH="${WIDTH:-640}"
HEIGHT="${HEIGHT:-360}"
FORMAT="${FORMAT:-YUYV}"
FRAMES="${FRAMES:-30}"
PORT="${PORT:-9602}"
START_DELAY_MS="${START_DELAY_MS:-750}"
MAX_ATTEMPTS="${MAX_ATTEMPTS:-10}"
RETRY_DELAY_MS="${RETRY_DELAY_MS:-200}"
TIMEOUT_MS="${TIMEOUT_MS:-2000}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${ROOT_DIR}/docs/logs/stage10_v4l2_delayed_receiver"
OUTPUT_DIR="${ROOT_DIR}/output/stage10_v4l2_delayed_receiver"
RECV_DIR="${OUTPUT_DIR}/recv"
SENDER_STATS_JSON="${OUTPUT_DIR}/pipeline_stats.json"
RECEIVER_STATS_JSON="${OUTPUT_DIR}/receiver_stats.json"
SENDER_LOG="${LOG_DIR}/sender.txt"
RECEIVER_LOG="${LOG_DIR}/receiver.txt"
EXPECTED_SIZE=$((WIDTH * HEIGHT * 2))

rm -rf "${OUTPUT_DIR}" "${LOG_DIR}"
mkdir -p "${RECV_DIR}" "${LOG_DIR}"

cleanup() {
    for pid in "${SENDER_PID:-}" "${RECEIVER_PID:-}"; do
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
        fi
    done
}
trap cleanup EXIT

(
    DELAY_SECONDS="$(awk -v ms="${START_DELAY_MS}" 'BEGIN {printf "%.3f", ms / 1000.0}')"
    sleep "${DELAY_SECONDS}"
    exec "${BUILD_DIR}/tcp_receiver" \
      --port "${PORT}" \
      --output "${RECV_DIR}" \
      --max-frames "${FRAMES}" \
      --max-sessions 1 \
      --max-payload-bytes 16777216 \
      --stats-output "${RECEIVER_STATS_JSON}"
) >"${RECEIVER_LOG}" 2>&1 &
RECEIVER_PID=$!

set +e
"${BUILD_DIR}/v4l2_capture" \
  --config "${ROOT_DIR}/config/v4l2_tcp_pipeline.conf" \
  --device "${DEVICE}" \
  --width "${WIDTH}" \
  --height "${HEIGHT}" \
  --format "${FORMAT}" \
  --mmap-buffers 4 \
  --pipeline-frames "${FRAMES}" \
  --ring-capacity 8 \
  --tcp-host 127.0.0.1 \
  --tcp-port "${PORT}" \
  --tcp-queue-capacity 8 \
  --tcp-connect-max-attempts "${MAX_ATTEMPTS}" \
  --tcp-connect-retry-delay-ms "${RETRY_DELAY_MS}" \
  --timeout-ms "${TIMEOUT_MS}" \
  --stats-output "${SENDER_STATS_JSON}" \
  >"${SENDER_LOG}" 2>&1
SENDER_STATUS=$?
wait "${RECEIVER_PID}"
RECEIVER_STATUS=$?
RECEIVER_PID=""
set -e

if [[ "${SENDER_STATUS}" -ne 0 || "${RECEIVER_STATUS}" -ne 0 ]]; then
    echo "[FAIL] V4L2 delayed-receiver test failed"
    echo "sender status=${SENDER_STATUS} receiver status=${RECEIVER_STATUS}"
    cat "${SENDER_LOG}"
    cat "${RECEIVER_LOG}"
    exit 1
fi

if [[ ! -f "${SENDER_STATS_JSON}" || ! -f "${RECEIVER_STATS_JSON}" ]]; then
    echo "[FAIL] expected JSON statistics were not created"
    exit 1
fi

FILE_COUNT="$(find "${RECV_DIR}" -type f -name '*.YUYV' | wc -l | tr -d '[:space:]')"
BAD_SIZE_COUNT="$(
    find "${RECV_DIR}" -type f -name '*.YUYV' -printf '%s\n' \
    | awk -v expected="${EXPECTED_SIZE}" '$1 != expected {count++} END {print count + 0}'
)"
TMP_COUNT="$(find "${OUTPUT_DIR}" -type f -name '*.tmp' | wc -l | tr -d '[:space:]')"

export SENDER_STATS_JSON RECEIVER_STATS_JSON FILE_COUNT BAD_SIZE_COUNT TMP_COUNT
python3 - <<'PY'
import json
import os
import sys

with open(os.environ['SENDER_STATS_JSON'], encoding='utf-8') as f:
    sender = json.load(f)
with open(os.environ['RECEIVER_STATS_JSON'], encoding='utf-8') as f:
    receiver = json.load(f)

attempts = sender.get('tcp_connect_attempts', 0)
retries = sender.get('tcp_connect_retries', -1)

checks = [
    (sender.get('produced_frames') == sender.get('consumed_frames'), 'produced/consumed mismatch'),
    (sender.get('ring_dropped_frames') == 0, 'main RingBuffer dropped frames'),
    (sender.get('tcp_queue_dropped') == 0, 'TCP RingBuffer dropped frames'),
    (sender.get('tcp_send_errors') == 0, 'TCP errors were reported'),
    (attempts > 1, f'expected delayed-start retry, got attempts={attempts}'),
    (retries == attempts - 1, f'retries={retries} does not match attempts={attempts}'),
    (sender.get('tcp_sent_frames') == receiver.get('received_frames'), 'sent/received mismatch'),
    (receiver.get('received_frames') == receiver.get('saved_files'), 'received/saved mismatch'),
    (receiver.get('crc_errors') == 0, 'CRC errors were reported'),
    (receiver.get('header_errors') == 0, 'header errors were reported'),
    (int(os.environ['FILE_COUNT']) == receiver.get('saved_files'), 'file count mismatch'),
    (int(os.environ['BAD_SIZE_COUNT']) == 0, 'unexpected-size files exist'),
    (int(os.environ['TMP_COUNT']) == 0, 'temporary files remain'),
    (sender.get('tcp_sent_frames') + sender.get('invalid_frames') == sender.get('consumed_frames'), 'valid accounting mismatch'),
]

for ok, message in checks:
    if not ok:
        print(f'[FAIL] {message}')
        sys.exit(1)

print(f'[PASS] V4L2 pipeline recovered after {retries} scheduled connection retries')
PY

echo "========== V4L2 Delayed Receiver Verification =========="
echo "sender exit code     : ${SENDER_STATUS}"
echo "receiver exit code   : ${RECEIVER_STATUS}"
echo "received files       : ${FILE_COUNT}"
echo "bad-size files       : ${BAD_SIZE_COUNT}"
echo "temporary files      : ${TMP_COUNT}"
echo "sender stats json    : ${SENDER_STATS_JSON}"
echo "receiver stats json  : ${RECEIVER_STATS_JSON}"
echo "========================================================"
