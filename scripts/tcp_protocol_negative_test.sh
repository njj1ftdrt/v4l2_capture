#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${ROOT_DIR}/docs/logs/protocol_negative"
OUTPUT_ROOT="${ROOT_DIR}/output/protocol_negative"
BASE_PORT="${BASE_PORT:-9200}"
MAX_PAYLOAD_BYTES="${MAX_PAYLOAD_BYTES:-16777216}"

FAULTS=(
  bad-magic
  bad-version
  bad-header-size
  zero-width
  oversized-payload
  yuyv-size-mismatch
)

mkdir -p "${LOG_DIR}"
rm -rf "${OUTPUT_ROOT}"
mkdir -p "${OUTPUT_ROOT}"

cleanup_pid=""
cleanup() {
    if [[ -n "${cleanup_pid}" ]] && kill -0 "${cleanup_pid}" 2>/dev/null; then
        kill "${cleanup_pid}" 2>/dev/null || true
        wait "${cleanup_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

for index in "${!FAULTS[@]}"; do
    fault="${FAULTS[$index]}"
    port=$((BASE_PORT + index))
    case_dir="${OUTPUT_ROOT}/${fault}"
    recv_dir="${case_dir}/recv"
    stats_json="${case_dir}/receiver_stats.json"
    receiver_log="${LOG_DIR}/${fault}_receiver.txt"
    sender_log="${LOG_DIR}/${fault}_sender.txt"

    mkdir -p "${case_dir}" "${recv_dir}"

    echo "[TEST] fault=${fault} port=${port}"

    "${BUILD_DIR}/tcp_receiver" \
      --port "${port}" \
      --output "${recv_dir}" \
      --max-frames 1 \
      --max-payload-bytes "${MAX_PAYLOAD_BYTES}" \
      --stats-output "${stats_json}" \
      >"${receiver_log}" 2>&1 &
    cleanup_pid=$!

    sleep 0.2

    set +e
    "${BUILD_DIR}/tcp_sender" \
      --host 127.0.0.1 \
      --port "${port}" \
      --frames 1 \
      --width 640 \
      --height 360 \
      --format YUYV \
      --header-fault "${fault}" \
      >"${sender_log}" 2>&1
    sender_status=$?

    wait "${cleanup_pid}"
    receiver_status=$?
    set -e
    cleanup_pid=""

    if [[ "${sender_status}" -ne 0 ]]; then
        echo "[FAIL] sender failed for ${fault}, status=${sender_status}"
        exit 1
    fi

    if [[ "${receiver_status}" -ne 3 ]]; then
        echo "[FAIL] receiver status for ${fault}: got=${receiver_status}, expected=3"
        exit 1
    fi

    if [[ ! -f "${stats_json}" ]]; then
        echo "[FAIL] receiver stats missing for ${fault}: ${stats_json}"
        exit 1
    fi

    export STATS_JSON="${stats_json}"
    export FAULT="${fault}"
    python3 - <<'PY'
import json
import os

expected_reason = {
    "bad-magic": "invalid magic",
    "bad-version": "unsupported protocol version",
    "bad-header-size": "unsupported header size",
    "zero-width": "zero width or height",
    "oversized-payload": "exceeds receiver limit",
    "yuyv-size-mismatch": "YUYV payload size mismatch",
}

with open(os.environ["STATS_JSON"], encoding="utf-8") as f:
    stats = json.load(f)

assert stats["received_frames"] == 0, stats
assert stats["received_bytes"] == 0, stats
assert stats["crc_errors"] == 0, stats
assert stats["header_errors"] == 1, stats
assert stats["rejected_frames"] == 1, stats
assert stats["saved_files"] == 0, stats
assert expected_reason[os.environ["FAULT"]] in stats["last_error"], stats
PY

    file_count="$(find "${recv_dir}" -type f -name '*.YUYV' 2>/dev/null | wc -l | tr -d '[:space:]')"
    if [[ "${file_count}" != "0" ]]; then
        echo "[FAIL] malformed header ${fault} unexpectedly produced ${file_count} file(s)"
        exit 1
    fi

    if [[ -e "${stats_json}.tmp" ]]; then
        echo "[FAIL] temporary stats file remains for ${fault}"
        exit 1
    fi

    echo "[PASS] ${fault} rejected before payload allocation"
done

echo "[PASS] all malformed-header tests passed."
