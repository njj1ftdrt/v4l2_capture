#!/usr/bin/env bash
set -euo pipefail

FRAMES="${FRAMES:-30}"
WIDTH="${WIDTH:-640}"
HEIGHT="${HEIGHT:-360}"
PORT="${PORT:-9700}"
INTERVAL_MS="${INTERVAL_MS:-10}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${ROOT_DIR}/docs/logs/stage11_latency"
OUTPUT_DIR="${ROOT_DIR}/output/stage11_latency/recv"
RECEIVER_STATS_JSON="${ROOT_DIR}/output/stage11_latency/receiver_stats.json"
RECEIVER_LOG="${LOG_DIR}/receiver.txt"
SENDER_LOG="${LOG_DIR}/sender.txt"
EXPECTED_SIZE=$((WIDTH * HEIGHT * 2))

mkdir -p "${LOG_DIR}"
rm -rf "${ROOT_DIR}/output/stage11_latency"
mkdir -p "${OUTPUT_DIR}"

cleanup() {
    if [[ -n "${RECEIVER_PID:-}" ]] && kill -0 "${RECEIVER_PID}" 2>/dev/null; then
        kill "${RECEIVER_PID}" 2>/dev/null || true
        wait "${RECEIVER_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

"${BUILD_DIR}/tcp_receiver" \
  --port "${PORT}" \
  --output "${OUTPUT_DIR}" \
  --max-frames "${FRAMES}" \
  --max-sessions 1 \
  --stats-output "${RECEIVER_STATS_JSON}" \
  >"${RECEIVER_LOG}" 2>&1 &
RECEIVER_PID=$!

sleep 0.5

set +e
"${BUILD_DIR}/tcp_sender" \
  --host 127.0.0.1 \
  --port "${PORT}" \
  --frames "${FRAMES}" \
  --width "${WIDTH}" \
  --height "${HEIGHT}" \
  --format YUYV \
  --interval-ms "${INTERVAL_MS}" \
  >"${SENDER_LOG}" 2>&1
SENDER_STATUS=$?
set -e

set +e
wait "${RECEIVER_PID}"
RECEIVER_STATUS=$?
set -e
RECEIVER_PID=""

if [[ "${SENDER_STATUS}" -ne 0 ]]; then
    echo "[FAIL] tcp_sender exited with status ${SENDER_STATUS}"
    cat "${SENDER_LOG}"
    exit 1
fi

if [[ "${RECEIVER_STATUS}" -ne 0 ]]; then
    echo "[FAIL] tcp_receiver exited with status ${RECEIVER_STATUS}"
    cat "${RECEIVER_LOG}"
    exit 1
fi

if [[ ! -f "${RECEIVER_STATS_JSON}" ]]; then
    echo "[FAIL] receiver stats JSON missing: ${RECEIVER_STATS_JSON}"
    exit 1
fi

FILE_COUNT="$(find "${OUTPUT_DIR}" -type f -name '*.YUYV' | wc -l | tr -d '[:space:]')"
BAD_SIZE_COUNT="$(
    find "${OUTPUT_DIR}" -type f -name '*.YUYV' -printf '%s\n' \
    | awk -v expected="${EXPECTED_SIZE}" '$1 != expected {count++} END {print count + 0}'
)"
TEMP_COUNT="$(find "${ROOT_DIR}/output/stage11_latency" -type f -name '*.tmp' | wc -l | tr -d '[:space:]')"

export RECEIVER_STATS_JSON FRAMES FILE_COUNT BAD_SIZE_COUNT TEMP_COUNT
RESULTS="$(python3 - <<'PY'
import json
import math
import os

with open(os.environ['RECEIVER_STATS_JSON'], encoding='utf-8') as f:
    stats = json.load(f)

frames = int(os.environ['FRAMES'])
required_zero = ['crc_errors', 'header_errors', 'rejected_frames', 'latency_clock_errors']
for key in required_zero:
    if stats[key] != 0:
        raise SystemExit(f'[FAIL] {key}={stats[key]}')

if stats['received_frames'] != frames:
    raise SystemExit(f"[FAIL] received_frames={stats['received_frames']} expected={frames}")
if stats['saved_files'] != frames:
    raise SystemExit(f"[FAIL] saved_files={stats['saved_files']} expected={frames}")
if stats['e2e_latency_samples'] != frames:
    raise SystemExit(
        f"[FAIL] e2e_latency_samples={stats['e2e_latency_samples']} expected={frames}"
    )

values = [
    stats['e2e_latency_min_us'],
    stats['e2e_latency_p50_us'],
    stats['e2e_latency_p95_us'],
    stats['e2e_latency_p99_us'],
    stats['e2e_latency_max_us'],
]
if not all(math.isfinite(v) and v >= 0 for v in values):
    raise SystemExit('[FAIL] latency values must be finite and non-negative')
if values != sorted(values):
    raise SystemExit(f'[FAIL] latency percentile ordering invalid: {values}')
if not (stats['e2e_latency_min_us'] <= stats['e2e_latency_mean_us'] <= stats['e2e_latency_max_us']):
    raise SystemExit('[FAIL] mean latency is outside min/max range')
if not math.isfinite(stats['e2e_latency_jitter_us']) or stats['e2e_latency_jitter_us'] < 0:
    raise SystemExit('[FAIL] jitter must be finite and non-negative')

pairs = {
    'LATENCY_SAMPLES': stats['e2e_latency_samples'],
    'LATENCY_MEAN_US': stats['e2e_latency_mean_us'],
    'LATENCY_P50_US': stats['e2e_latency_p50_us'],
    'LATENCY_P95_US': stats['e2e_latency_p95_us'],
    'LATENCY_P99_US': stats['e2e_latency_p99_us'],
    'LATENCY_MAX_US': stats['e2e_latency_max_us'],
    'LATENCY_JITTER_US': stats['e2e_latency_jitter_us'],
}
for key, value in pairs.items():
    print(f'{key}={value}')
PY
)"
eval "${RESULTS}"

if [[ "${FILE_COUNT}" != "${FRAMES}" ]]; then
    echo "[FAIL] received file count ${FILE_COUNT}, expected ${FRAMES}"
    exit 1
fi
if [[ "${BAD_SIZE_COUNT}" != "0" ]]; then
    echo "[FAIL] ${BAD_SIZE_COUNT} file(s) have unexpected size"
    exit 1
fi
if [[ "${TEMP_COUNT}" != "0" ]]; then
    echo "[FAIL] temporary output files remain"
    exit 1
fi

echo "[PASS] protocol v3 latency statistics are complete and ordered"
echo "========== Latency Verification =========="
echo "sender exit code : ${SENDER_STATUS}"
echo "receiver exit code: ${RECEIVER_STATUS}"
echo "latency samples   : ${LATENCY_SAMPLES}"
echo "mean us           : ${LATENCY_MEAN_US}"
echo "p50 us            : ${LATENCY_P50_US}"
echo "p95 us            : ${LATENCY_P95_US}"
echo "p99 us            : ${LATENCY_P99_US}"
echo "max us            : ${LATENCY_MAX_US}"
echo "jitter us         : ${LATENCY_JITTER_US}"
echo "received files    : ${FILE_COUNT}"
echo "bad-size files    : ${BAD_SIZE_COUNT}"
echo "temporary files   : ${TEMP_COUNT}"
echo "stats json        : ${RECEIVER_STATS_JSON}"
echo "=========================================="
