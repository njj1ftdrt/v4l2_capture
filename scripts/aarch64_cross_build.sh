#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-aarch64-cross}"
STAGE_DIR="${STAGE_DIR:-${ROOT_DIR}/output/aarch64-cross/stage}"
EVIDENCE_DIR="${EVIDENCE_DIR:-${ROOT_DIR}/output/aarch64-cross/evidence}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-${ROOT_DIR}/cmake/toolchains/aarch64-linux-gnu.cmake}"
JOBS="${JOBS:-$(nproc)}"

required=(cmake aarch64-linux-gnu-g++ file sha256sum tar)
for cmd in "${required[@]}"; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "[ERROR] Missing command: ${cmd}" >&2
        echo "[INFO] Debian/Ubuntu: sudo apt-get install g++-aarch64-linux-gnu cmake ninja-build file" >&2
        exit 2
    fi
done

rm -rf "${BUILD_DIR}" "${STAGE_DIR}" "${EVIDENCE_DIR}"
mkdir -p "${BUILD_DIR}" "${STAGE_DIR}" "${EVIDENCE_DIR}"

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    "${GENERATOR_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DBUILD_TESTING=ON \
    2>&1 | tee "${EVIDENCE_DIR}/configure.log"

cmake --build "${BUILD_DIR}" -j"${JOBS}" \
    2>&1 | tee "${EVIDENCE_DIR}/build.log"

binaries=(v4l2_capture tcp_sender tcp_receiver protocol_fixture test_ring_buffer test_frame_protocol test_latency_stats test_tcp_send test_capture_recovery)
: > "${EVIDENCE_DIR}/file.txt"
for binary in "${binaries[@]}"; do
    path="${BUILD_DIR}/${binary}"
    if [[ ! -x "${path}" ]]; then
        echo "[ERROR] Expected binary missing: ${path}" >&2
        exit 3
    fi
    description="$(file -b "${path}")"
    printf '%s: %s\n' "${binary}" "${description}" | tee -a "${EVIDENCE_DIR}/file.txt"
    if [[ "${description}" != *"ARM aarch64"* ]]; then
        echo "[ERROR] ${binary} is not an AArch64 ELF" >&2
        exit 4
    fi
done

DESTDIR="${STAGE_DIR}" cmake --install "${BUILD_DIR}" --prefix /usr/local \
    2>&1 | tee "${EVIDENCE_DIR}/install.log"

(
    cd "${STAGE_DIR}"
    find . -type f -print0 | sort -z | xargs -0 sha256sum
) > "${EVIDENCE_DIR}/sha256sums.txt"

cat > "${EVIDENCE_DIR}/manifest.json" <<EOF_MANIFEST
{
  "target_architecture": "aarch64",
  "build_type": "${BUILD_TYPE}",
  "compiler": "$(aarch64-linux-gnu-g++ -dumpfullversion -dumpversion)",
  "toolchain_file": "${TOOLCHAIN_FILE}",
  "staged_prefix": "/usr/local"
}
EOF_MANIFEST

PACKAGE="${ROOT_DIR}/output/v4l2_capture-aarch64-${BUILD_TYPE,,}.tar.gz"
PACKAGE_DIR="$(dirname "${PACKAGE}")"
PACKAGE_NAME="$(basename "${PACKAGE}")"
mkdir -p "${PACKAGE_DIR}"
tar -C "${STAGE_DIR}" -czf "${PACKAGE}" .
(
    cd "${PACKAGE_DIR}"
    sha256sum "${PACKAGE_NAME}" | tee "${PACKAGE_NAME}.sha256"
)

echo "[PASS] AArch64 cross build completed"
echo "[INFO] Package: ${PACKAGE}"
echo "[INFO] Evidence: ${EVIDENCE_DIR}"
