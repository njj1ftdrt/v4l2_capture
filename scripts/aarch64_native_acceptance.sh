#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-aarch64-native}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/output/aarch64-native}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
FRAMES="${FRAMES:-100}"
WIDTH="${WIDTH:-64}"
HEIGHT="${HEIGHT:-48}"
if [[ -n "${PORT:-}" ]]; then
    PORT_VALUE="${PORT}"
else
    PORT_VALUE="$(python3 - <<'PY_PORT'
import socket
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY_PORT
)"
fi
JOBS="${JOBS:-$(nproc)}"

arch="$(uname -m)"
case "${arch}" in
    aarch64|arm64) ;;
    *)
        echo "[ERROR] Native ARM64 acceptance requires aarch64/arm64, got: ${arch}" >&2
        exit 2
        ;;
esac

required=(cmake python3 file sha256sum)
for cmd in "${required[@]}"; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "[ERROR] Missing command: ${cmd}" >&2
        exit 3
    fi
done

rm -rf "${BUILD_DIR}" "${OUTPUT_DIR}"
mkdir -p "${BUILD_DIR}" "${OUTPUT_DIR}/tcp_recv" "${OUTPUT_DIR}/evidence"

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    "${GENERATOR_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DBUILD_TESTING=ON \
    2>&1 | tee "${OUTPUT_DIR}/evidence/configure.log"
cmake --build "${BUILD_DIR}" -j"${JOBS}" \
    2>&1 | tee "${OUTPUT_DIR}/evidence/build.log"
ctest --test-dir "${BUILD_DIR}" --output-on-failure \
    2>&1 | tee "${OUTPUT_DIR}/evidence/ctest.log"

receiver_log="${OUTPUT_DIR}/receiver.log"
sender_log="${OUTPUT_DIR}/sender.log"
stats_json="${OUTPUT_DIR}/receiver_stats.json"

"${BUILD_DIR}/tcp_receiver" \
    --port "${PORT_VALUE}" \
    --output "${OUTPUT_DIR}/tcp_recv" \
    --stats-output "${stats_json}" \
    --max-frames "${FRAMES}" \
    >"${receiver_log}" 2>&1 &
receiver_pid=$!

cleanup() {
    if kill -0 "${receiver_pid}" >/dev/null 2>&1; then
        kill "${receiver_pid}" >/dev/null 2>&1 || true
        wait "${receiver_pid}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

for _ in $(seq 1 50); do
    if grep -q "listening on port" "${receiver_log}" 2>/dev/null; then
        break
    fi
    if ! kill -0 "${receiver_pid}" >/dev/null 2>&1; then
        echo "[ERROR] Receiver exited before listening" >&2
        cat "${receiver_log}" >&2
        exit 4
    fi
    sleep 0.1
done

"${BUILD_DIR}/tcp_sender" \
    --host 127.0.0.1 \
    --port "${PORT_VALUE}" \
    --frames "${FRAMES}" \
    --width "${WIDTH}" \
    --height "${HEIGHT}" \
    --interval-ms 1 \
    --connect-max-attempts 20 \
    --connect-retry-delay-ms 100 \
    --send-timeout-ms 2000 \
    >"${sender_log}" 2>&1

wait "${receiver_pid}"
trap - EXIT

python3 - "${stats_json}" "${FRAMES}" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
expected = int(sys.argv[2])
data = json.loads(path.read_text())
checks = {
    "received_frames": (data.get("received_frames"), expected),
    "crc_errors": (data.get("crc_errors"), 0),
    "header_errors": (data.get("header_errors"), 0),
    "rejected_frames": (data.get("rejected_frames"), 0),
}
failed = []
for name, (actual, wanted) in checks.items():
    if actual != wanted:
        failed.append(f"{name}: actual={actual} expected={wanted}")
if failed:
    raise SystemExit("; ".join(failed))
print("[PASS] TCP loopback accounting:", ", ".join(f"{k}={v[0]}" for k, v in checks.items()))
PY

{
    echo "architecture=${arch}"
    uname -a
    if command -v lscpu >/dev/null 2>&1; then lscpu; fi
    echo "cmake=$(cmake --version | head -n1)"
    echo "cxx=$(${CXX:-c++} --version | head -n1)"
    for binary in v4l2_capture tcp_sender tcp_receiver; do
        printf '%s: ' "${binary}"
        file -b "${BUILD_DIR}/${binary}"
    done
} > "${OUTPUT_DIR}/evidence/platform.txt"

sha256sum "${BUILD_DIR}/v4l2_capture" "${BUILD_DIR}/tcp_sender" "${BUILD_DIR}/tcp_receiver" \
    > "${OUTPUT_DIR}/evidence/binary_sha256sums.txt"

python3 - "${OUTPUT_DIR}" "${arch}" "${BUILD_TYPE}" "${FRAMES}" <<'PY'
import json
import sys
from pathlib import Path

out = Path(sys.argv[1])
manifest = {
    "architecture": sys.argv[2],
    "build_type": sys.argv[3],
    "ctest_passed": True,
    "tcp_loopback_frames": int(sys.argv[4]),
    "receiver_stats": str(out / "receiver_stats.json"),
}
(out / "evidence" / "acceptance_manifest.json").write_text(
    json.dumps(manifest, indent=2) + "\n"
)
PY

echo "[PASS] Native ARM64 build, unit tests, and TCP loopback completed"
echo "[INFO] Evidence: ${OUTPUT_DIR}/evidence"
