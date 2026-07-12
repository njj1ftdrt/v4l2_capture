#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${ROOT_DIR}/docs/logs/stage10_connect_retry"
OUTPUT_DIR="${ROOT_DIR}/output/stage10_connect_retry"
SUCCESS_PORT="${SUCCESS_PORT:-9600}"
FAIL_PORT="${FAIL_PORT:-9601}"
START_DELAY_MS="${START_DELAY_MS:-750}"
RETRY_DELAY_MS="${RETRY_DELAY_MS:-200}"
MAX_ATTEMPTS="${MAX_ATTEMPTS:-10}"

SUCCESS_SENDER_LOG="${LOG_DIR}/delayed_receiver_sender.txt"
SUCCESS_RECEIVER_LOG="${LOG_DIR}/delayed_receiver_receiver.txt"
FAIL_SENDER_LOG="${LOG_DIR}/exhausted_sender.txt"
SUCCESS_STATS_JSON="${OUTPUT_DIR}/delayed_receiver/receiver_stats.json"
SUCCESS_RECV_DIR="${OUTPUT_DIR}/delayed_receiver/recv"
EXPECTED_SIZE=$((640 * 360 * 2))

rm -rf "${OUTPUT_DIR}" "${LOG_DIR}"
mkdir -p "${SUCCESS_RECV_DIR}" "${LOG_DIR}"

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
      --port "${SUCCESS_PORT}" \
      --output "${SUCCESS_RECV_DIR}" \
      --max-frames 3 \
      --max-sessions 1 \
      --max-payload-bytes 16777216 \
      --stats-output "${SUCCESS_STATS_JSON}"
) >"${SUCCESS_RECEIVER_LOG}" 2>&1 &
RECEIVER_PID=$!

set +e
"${BUILD_DIR}/tcp_sender" \
  --host 127.0.0.1 \
  --port "${SUCCESS_PORT}" \
  --frames 3 \
  --width 640 \
  --height 360 \
  --format YUYV \
  --connect-max-attempts "${MAX_ATTEMPTS}" \
  --connect-retry-delay-ms "${RETRY_DELAY_MS}" \
  >"${SUCCESS_SENDER_LOG}" 2>&1
SUCCESS_SENDER_STATUS=$?
wait "${RECEIVER_PID}"
SUCCESS_RECEIVER_STATUS=$?
RECEIVER_PID=""
set -e

if [[ "${SUCCESS_SENDER_STATUS}" -ne 0 || "${SUCCESS_RECEIVER_STATUS}" -ne 0 ]]; then
    echo "[FAIL] delayed receiver recovery failed"
    echo "sender status=${SUCCESS_SENDER_STATUS} receiver status=${SUCCESS_RECEIVER_STATUS}"
    cat "${SUCCESS_SENDER_LOG}"
    cat "${SUCCESS_RECEIVER_LOG}"
    exit 1
fi

if [[ ! -f "${SUCCESS_STATS_JSON}" ]]; then
    echo "[FAIL] delayed receiver stats json was not created"
    exit 1
fi

CONNECTED_ATTEMPT="$(
    sed -n 's/.*connected attempt=\([0-9][0-9]*\)\/.*/\1/p' "${SUCCESS_SENDER_LOG}" \
    | tail -1
)"
WARNING_COUNT="$(grep -c '^\[WARN\] connect attempt ' "${SUCCESS_SENDER_LOG}" || true)"
FILE_COUNT="$(find "${SUCCESS_RECV_DIR}" -type f -name '*.YUYV' | wc -l | tr -d '[:space:]')"
BAD_SIZE_COUNT="$(
    find "${SUCCESS_RECV_DIR}" -type f -name '*.YUYV' -printf '%s\n' \
    | awk -v expected="${EXPECTED_SIZE}" '$1 != expected {count++} END {print count + 0}'
)"

export SUCCESS_STATS_JSON CONNECTED_ATTEMPT WARNING_COUNT FILE_COUNT BAD_SIZE_COUNT
python3 - <<'PY'
import json
import os
import sys

attempt = int(os.environ['CONNECTED_ATTEMPT'] or '0')
warning_count = int(os.environ['WARNING_COUNT'])
file_count = int(os.environ['FILE_COUNT'])
bad_size_count = int(os.environ['BAD_SIZE_COUNT'])

with open(os.environ['SUCCESS_STATS_JSON'], encoding='utf-8') as f:
    receiver = json.load(f)

if attempt <= 1:
    print(f'[FAIL] expected retry recovery after more than one attempt, got {attempt}')
    sys.exit(1)
if warning_count != attempt - 1:
    print(f'[FAIL] warning count {warning_count} does not match retries {attempt - 1}')
    sys.exit(1)
if receiver.get('received_frames') != 3 or receiver.get('saved_files') != 3:
    print(f'[FAIL] receiver frame statistics are incorrect: {receiver}')
    sys.exit(1)
if receiver.get('crc_errors') != 0 or receiver.get('header_errors') != 0:
    print(f'[FAIL] receiver reported protocol errors: {receiver}')
    sys.exit(1)
if file_count != 3 or bad_size_count != 0:
    print(f'[FAIL] received files={file_count}, bad_size_files={bad_size_count}')
    sys.exit(1)

print(f'[PASS] delayed receiver recovered on connection attempt {attempt}')
PY

set +e
"${BUILD_DIR}/tcp_sender" \
  --host 127.0.0.1 \
  --port "${FAIL_PORT}" \
  --frames 1 \
  --connect-max-attempts 3 \
  --connect-retry-delay-ms 100 \
  >"${FAIL_SENDER_LOG}" 2>&1
FAIL_SENDER_STATUS=$?
set -e

if [[ "${FAIL_SENDER_STATUS}" -eq 0 ]]; then
    echo "[FAIL] sender unexpectedly connected in exhaustion test"
    exit 1
fi

if ! grep -q 'failed after 3 attempt(s)' "${FAIL_SENDER_LOG}"; then
    echo "[FAIL] exhaustion log does not report exactly three attempts"
    cat "${FAIL_SENDER_LOG}"
    exit 1
fi

FAIL_WARNING_COUNT="$(grep -c '^\[WARN\] connect attempt ' "${FAIL_SENDER_LOG}" || true)"
if [[ "${FAIL_WARNING_COUNT}" != "2" ]]; then
    echo "[FAIL] expected two scheduled retries before the final failed attempt"
    cat "${FAIL_SENDER_LOG}"
    exit 1
fi

echo "[PASS] exhausted connection attempts returned a non-zero status"
echo "========== TCP Connect Retry Verification =========="
echo "delayed-start sender status : ${SUCCESS_SENDER_STATUS}"
echo "delayed-start receiver status: ${SUCCESS_RECEIVER_STATUS}"
echo "successful attempt          : ${CONNECTED_ATTEMPT}"
echo "scheduled retries           : ${WARNING_COUNT}"
echo "received files              : ${FILE_COUNT}"
echo "bad-size files              : ${BAD_SIZE_COUNT}"
echo "exhaustion sender status    : ${FAIL_SENDER_STATUS}"
echo "===================================================="
