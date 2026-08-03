#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROLE="${1:-${ROLE:-}}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
PORT="${PORT:-9000}"
FRAMES="${FRAMES:-300}"
WIDTH="${WIDTH:-64}"
HEIGHT="${HEIGHT:-48}"
INTERVAL_MS="${INTERVAL_MS:-1}"
SEND_TIMEOUT_MS="${SEND_TIMEOUT_MS:-3000}"
SAVE_PAYLOADS="${SAVE_PAYLOADS:-1}"
TARGET_HOST="${TARGET_HOST:-}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/output/cross-machine-${ROLE:-unknown}}"

usage() {
    cat <<'USAGE'
Usage:
  Receiver host:
    BUILD_DIR=build-arm64 FRAMES=300 PORT=9000 \
      ./scripts/cross_machine_tcp_acceptance.sh receiver

  Sender host:
    BUILD_DIR=build TARGET_HOST=ARM_IP FRAMES=300 PORT=9000 \
      ./scripts/cross_machine_tcp_acceptance.sh sender
USAGE
}

if [[ "${ROLE}" != "receiver" && "${ROLE}" != "sender" ]]; then
    usage >&2
    exit 2
fi

required=(python3 sha256sum file)
for cmd in "${required[@]}"; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "[ERROR] Missing command: ${cmd}" >&2
        exit 3
    fi
done

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}/evidence"

{
    echo "role=${ROLE}"
    echo "architecture=$(uname -m)"
    echo "hostname=$(hostname)"
    echo "utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    uname -a
    if command -v ip >/dev/null 2>&1; then
        ip -brief address || true
    fi
} > "${OUTPUT_DIR}/evidence/platform.txt"

if [[ "${ROLE}" == "receiver" ]]; then
    receiver="${BUILD_DIR}/tcp_receiver"
    if [[ ! -x "${receiver}" ]]; then
        echo "[ERROR] Receiver binary not found: ${receiver}" >&2
        exit 4
    fi

    file -b "${receiver}" > "${OUTPUT_DIR}/evidence/binary.txt"
    sha256sum "${receiver}" > "${OUTPUT_DIR}/evidence/binary.sha256"

    stats_json="${OUTPUT_DIR}/receiver_stats.json"
    echo "[INFO] Receiver waiting on 0.0.0.0:${PORT}, frames=${FRAMES}"
    receiver_args=(
        --port "${PORT}"
        --max-frames "${FRAMES}"
        --max-sessions 1
        --output "${OUTPUT_DIR}/frames"
        --stats-output "${stats_json}"
    )
    if [[ "${SAVE_PAYLOADS}" == "0" ]]; then
        receiver_args+=(--discard-payload)
    fi
    "${receiver}" "${receiver_args[@]}" \
        2>&1 | tee "${OUTPUT_DIR}/receiver.log"

    python3 - "${stats_json}" "${FRAMES}" "${OUTPUT_DIR}/evidence/acceptance_manifest.json" <<'PY'
import json
import sys
from pathlib import Path

stats_path = Path(sys.argv[1])
expected = int(sys.argv[2])
manifest_path = Path(sys.argv[3])
data = json.loads(stats_path.read_text())
checks = {
    "received_frames": expected,
    "crc_errors": 0,
    "header_errors": 0,
    "rejected_frames": 0,
    "accepted_sessions": 1,
    "completed_sessions": 1,
}
failures = []
for key, wanted in checks.items():
    actual = data.get(key)
    if actual != wanted:
        failures.append(f"{key}: actual={actual} expected={wanted}")
if failures:
    raise SystemExit("; ".join(failures))
manifest = {
    "role": "receiver",
    "passed": True,
    "checks": checks,
    "received_bytes": data.get("received_bytes"),
    "peer_disconnects": data.get("peer_disconnects"),
    "last_error": data.get("last_error"),
    "saved_files": data.get("saved_files"),
}
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
print("[PASS] cross-machine receiver accounting:", ", ".join(f"{k}={v}" for k, v in checks.items()))
PY

    echo "[PASS] Receiver accepted ${FRAMES} frames with zero protocol errors"
else
    sender="${BUILD_DIR}/tcp_sender"
    if [[ ! -x "${sender}" ]]; then
        echo "[ERROR] Sender binary not found: ${sender}" >&2
        exit 5
    fi
    if [[ -z "${TARGET_HOST}" ]]; then
        echo "[ERROR] TARGET_HOST is required for sender role" >&2
        exit 6
    fi

    file -b "${sender}" > "${OUTPUT_DIR}/evidence/binary.txt"
    sha256sum "${sender}" > "${OUTPUT_DIR}/evidence/binary.sha256"

    echo "[INFO] Sender connecting to ${TARGET_HOST}:${PORT}, frames=${FRAMES}"
    "${sender}" \
        --host "${TARGET_HOST}" \
        --port "${PORT}" \
        --frames "${FRAMES}" \
        --width "${WIDTH}" \
        --height "${HEIGHT}" \
        --interval-ms "${INTERVAL_MS}" \
        --connect-max-attempts 20 \
        --connect-retry-delay-ms 250 \
        --send-timeout-ms "${SEND_TIMEOUT_MS}" \
        2>&1 | tee "${OUTPUT_DIR}/sender.log"

    python3 - \
        "${OUTPUT_DIR}/sender.log" \
        "${FRAMES}" \
        "${TARGET_HOST}" \
        "${PORT}" \
        "${OUTPUT_DIR}/evidence/acceptance_manifest.json" <<'PY'
import json
import re
import sys
from pathlib import Path

log_path = Path(sys.argv[1])
expected = int(sys.argv[2])
target = sys.argv[3]
port = int(sys.argv[4])
manifest_path = Path(sys.argv[5])
text = log_path.read_text()
match = re.search(r"sent frames\s*:\s*(\d+)", text)
if not match:
    raise SystemExit("sender statistics did not contain sent frames")
actual = int(match.group(1))
if actual != expected:
    raise SystemExit(f"sent_frames: actual={actual} expected={expected}")
manifest = {
    "role": "sender",
    "passed": True,
    "target_host": target,
    "port": port,
    "sent_frames": actual,
}
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
print(f"[PASS] cross-machine sender accounting: sent_frames={actual}")
PY

    echo "[PASS] Sender completed ${FRAMES} frames to ${TARGET_HOST}:${PORT}"
fi

echo "[INFO] Evidence: ${OUTPUT_DIR}"
