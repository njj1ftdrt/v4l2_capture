#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-arm64-cloud}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/output/arm64-cloud-prepare}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
INSTALL_DEPS="${INSTALL_DEPS:-1}"
SOURCE_COMMIT="${SOURCE_COMMIT:-unknown}"
JOBS="${JOBS:-$(nproc)}"

arch="$(uname -m)"
case "${arch}" in
    aarch64|arm64) ;;
    *)
        echo "[ERROR] Native ARM64 host required; uname -m=${arch}" >&2
        exit 2
        ;;
esac

if [[ "${INSTALL_DEPS}" == "1" ]]; then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "[ERROR] sudo is required when INSTALL_DEPS=1" >&2
        exit 3
    fi
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
        build-essential cmake ninja-build file python3 iproute2 ca-certificates
fi

required=(cmake file python3 sha256sum ss)
for cmd in "${required[@]}"; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "[ERROR] Missing command: ${cmd}" >&2
        exit 4
    fi
done

rm -rf "${BUILD_DIR}" "${OUTPUT_DIR}"
mkdir -p "${BUILD_DIR}" "${OUTPUT_DIR}/evidence"

{
    echo "architecture=${arch}"
    echo "hostname=$(hostname)"
    echo "source_commit=${SOURCE_COMMIT}"
    echo "utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "build_type=${BUILD_TYPE}"
    uname -a
    lscpu || true
} > "${OUTPUT_DIR}/evidence/platform.txt"

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

binaries=(v4l2_capture tcp_sender tcp_receiver protocol_fixture)
: > "${OUTPUT_DIR}/evidence/binaries.txt"
: > "${OUTPUT_DIR}/evidence/binaries.sha256"
for binary in "${binaries[@]}"; do
    path="${BUILD_DIR}/${binary}"
    [[ -x "${path}" ]] || { echo "[ERROR] Missing binary: ${path}" >&2; exit 5; }
    desc="$(file -b "${path}")"
    printf '%s: %s\n' "${binary}" "${desc}" | tee -a "${OUTPUT_DIR}/evidence/binaries.txt"
    if [[ "${desc}" != *"ARM aarch64"* ]]; then
        echo "[ERROR] ${binary} is not an ARM aarch64 ELF" >&2
        exit 6
    fi
    (cd "${BUILD_DIR}" && sha256sum "${binary}") >> "${OUTPUT_DIR}/evidence/binaries.sha256"
done

python3 - "${OUTPUT_DIR}/evidence/manifest.json" "${SOURCE_COMMIT}" "${arch}" "${BUILD_TYPE}" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
path.write_text(json.dumps({
    "passed": True,
    "architecture": sys.argv[3],
    "source_commit": sys.argv[2],
    "build_type": sys.argv[4],
    "ctest_passed": True,
}, indent=2) + "\n")
PY

echo "[PASS] ARM64 cloud host prepared"
echo "[INFO] Build: ${BUILD_DIR}"
echo "[INFO] Evidence: ${OUTPUT_DIR}"
