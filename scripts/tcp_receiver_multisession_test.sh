#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-9500}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${ROOT_DIR}/docs/logs/stage9_multisession"
OUTPUT_DIR="${ROOT_DIR}/output/stage9_multisession"
RECV_DIR="${OUTPUT_DIR}/recv"
STATS_JSON="${OUTPUT_DIR}/receiver_stats.json"
RECEIVER_LOG="${LOG_DIR}/receiver.txt"
SENDER1_LOG="${LOG_DIR}/sender_session1.txt"
SENDER2_LOG="${LOG_DIR}/sender_session2.txt"
EXPECTED_SIZE=$((640 * 360 * 2))

rm -rf "${OUTPUT_DIR}" "${LOG_DIR}"
mkdir -p "${RECV_DIR}" "${LOG_DIR}"

cleanup() {
    if [[ -n "${RECEIVER_PID:-}" ]] && kill -0 "${RECEIVER_PID}" 2>/dev/null; then
        kill "${RECEIVER_PID}" 2>/dev/null || true
        wait "${RECEIVER_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

"${BUILD_DIR}/tcp_receiver" \
  --port "${PORT}" \
  --output "${RECV_DIR}" \
  --max-frames 5 \
  --max-sessions 2 \
  --max-payload-bytes 16777216 \
  --stats-output "${STATS_JSON}" \
  >"${RECEIVER_LOG}" 2>&1 &
RECEIVER_PID=$!

sleep 1

if ! kill -0 "${RECEIVER_PID}" 2>/dev/null; then
    echo "[FAIL] receiver exited before the first sender connected"
    cat "${RECEIVER_LOG}"
    wait "${RECEIVER_PID}" 2>/dev/null || true
    exit 1
fi

"${BUILD_DIR}/tcp_sender" \
  --host 127.0.0.1 \
  --port "${PORT}" \
  --frames 2 \
  --width 640 \
  --height 360 \
  --format YUYV \
  >"${SENDER1_LOG}" 2>&1

sleep 1

if ! kill -0 "${RECEIVER_PID}" 2>/dev/null; then
    echo "[FAIL] receiver exited after the first session"
    cat "${RECEIVER_LOG}"
    exit 1
fi

"${BUILD_DIR}/tcp_sender" \
  --host 127.0.0.1 \
  --port "${PORT}" \
  --frames 3 \
  --width 640 \
  --height 360 \
  --format YUYV \
  >"${SENDER2_LOG}" 2>&1

set +e
wait "${RECEIVER_PID}"
RECEIVER_STATUS=$?
set -e
RECEIVER_PID=""

if [[ "${RECEIVER_STATUS}" -ne 0 ]]; then
    echo "[FAIL] receiver exited with status ${RECEIVER_STATUS}"
    cat "${RECEIVER_LOG}"
    exit 1
fi

if [[ ! -f "${STATS_JSON}" ]]; then
    echo "[FAIL] receiver stats json was not created"
    exit 1
fi

FILE_COUNT="$(find "${RECV_DIR}" -type f -name '*.YUYV' | wc -l | tr -d '[:space:]')"
BAD_SIZE_COUNT="$(
    find "${RECV_DIR}" -type f -name '*.YUYV' -printf '%s\n' \
    | awk -v expected="${EXPECTED_SIZE}" '$1 != expected {count++} END {print count + 0}'
)"
TMP_COUNT="$(find "${OUTPUT_DIR}" -type f -name '*.tmp' | wc -l | tr -d '[:space:]')"

export STATS_JSON FILE_COUNT BAD_SIZE_COUNT TMP_COUNT
python3 - <<'PY'
import json
import os
import sys

with open(os.environ['STATS_JSON'], encoding='utf-8') as f:
    stats = json.load(f)

expected = {
    'received_frames': 5,
    'received_bytes': 5 * 640 * 360 * 2,
    'crc_errors': 0,
    'header_errors': 0,
    'rejected_frames': 0,
    'saved_files': 5,
    'accepted_sessions': 2,
    'completed_sessions': 2,
    'peer_disconnects': 1,
}

for key, value in expected.items():
    if stats.get(key) != value:
        print(f'[FAIL] {key}: expected {value}, got {stats.get(key)}')
        sys.exit(1)

if stats.get('last_error') != '':
    print(f"[FAIL] last_error is not empty: {stats.get('last_error')}")
    sys.exit(1)

if int(os.environ['FILE_COUNT']) != 5:
    print(f"[FAIL] expected 5 received files, got {os.environ['FILE_COUNT']}")
    sys.exit(1)

if int(os.environ['BAD_SIZE_COUNT']) != 0:
    print(f"[FAIL] unexpected-size files: {os.environ['BAD_SIZE_COUNT']}")
    sys.exit(1)

if int(os.environ['TMP_COUNT']) != 0:
    print(f"[FAIL] temporary files remain: {os.environ['TMP_COUNT']}")
    sys.exit(1)

print('[PASS] receiver accepted two sequential sessions and accumulated five valid frames')
PY

echo "========== Multi-session Verification =========="
echo "receiver exit code : ${RECEIVER_STATUS}"
echo "received files     : ${FILE_COUNT}"
echo "bad size files     : ${BAD_SIZE_COUNT}"
echo "temporary files    : ${TMP_COUNT}"
echo "stats json         : ${STATS_JSON}"
echo "================================================"
