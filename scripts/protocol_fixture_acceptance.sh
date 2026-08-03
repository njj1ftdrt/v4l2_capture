#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-protocol-fixture}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/output/protocol-fixture}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
LABEL="${LABEL:-$(uname -m)}"
FRAMES="${FRAMES:-32}"
CROSS_ARCH_FIXTURE="${CROSS_ARCH_FIXTURE:-}"
FIXTURE_TOOL="${FIXTURE_TOOL:-}"
JOBS="${JOBS:-$(nproc)}"

if [[ ! "${LABEL}" =~ ^[A-Za-z0-9_.-]+$ ]]; then
    echo "[ERROR] LABEL contains unsupported characters: ${LABEL}" >&2
    exit 2
fi

required=(cmake file sha256sum cmp python3)
for cmd in "${required[@]}"; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "[ERROR] Missing command: ${cmd}" >&2
        exit 3
    fi
done

mkdir -p "${OUTPUT_DIR}/evidence"

if [[ -z "${FIXTURE_TOOL}" ]]; then
    rm -rf "${BUILD_DIR}"
    generator_args=()
    if command -v ninja >/dev/null 2>&1; then
        generator_args=(-G Ninja)
    fi

    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
        "${generator_args[@]}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DBUILD_TESTING=ON \
        2>&1 | tee "${OUTPUT_DIR}/evidence/configure.log"
    cmake --build "${BUILD_DIR}" --target protocol_fixture test_frame_protocol -j"${JOBS}" \
        2>&1 | tee "${OUTPUT_DIR}/evidence/build.log"
    ctest --test-dir "${BUILD_DIR}" -R '^test_frame_protocol$' --output-on-failure \
        2>&1 | tee "${OUTPUT_DIR}/evidence/ctest_protocol.log"
    FIXTURE_TOOL="${BUILD_DIR}/protocol_fixture"
fi

if [[ ! -x "${FIXTURE_TOOL}" ]]; then
    echo "[ERROR] protocol_fixture executable not found: ${FIXTURE_TOOL}" >&2
    exit 4
fi

fixture_path="${OUTPUT_DIR}/${LABEL}_protocol_fixture.bin"
write_stats="${OUTPUT_DIR}/evidence/${LABEL}_write_stats.json"
verify_stats="${OUTPUT_DIR}/evidence/${LABEL}_verify_stats.json"

"${FIXTURE_TOOL}" \
    --write "${fixture_path}" \
    --frames "${FRAMES}" \
    --stats-output "${write_stats}" \
    2>&1 | tee "${OUTPUT_DIR}/evidence/${LABEL}_write.log"

"${FIXTURE_TOOL}" \
    --verify "${fixture_path}" \
    --frames "${FRAMES}" \
    --stats-output "${verify_stats}" \
    2>&1 | tee "${OUTPUT_DIR}/evidence/${LABEL}_self_verify.log"

(
    cd "${OUTPUT_DIR}"
    sha256sum "$(basename "${fixture_path}")"
) > "${OUTPUT_DIR}/evidence/${LABEL}_fixture.sha256"

cross_verified=false
identical_bytes=false
cross_fixture_sha256=""
if [[ -n "${CROSS_ARCH_FIXTURE}" ]]; then
    if [[ ! -f "${CROSS_ARCH_FIXTURE}" ]]; then
        echo "[ERROR] Cross-architecture fixture not found: ${CROSS_ARCH_FIXTURE}" >&2
        exit 5
    fi

    "${FIXTURE_TOOL}" \
        --verify "${CROSS_ARCH_FIXTURE}" \
        --frames "${FRAMES}" \
        --stats-output "${OUTPUT_DIR}/evidence/${LABEL}_cross_verify_stats.json" \
        2>&1 | tee "${OUTPUT_DIR}/evidence/${LABEL}_cross_verify.log"
    cross_verified=true

    if ! cmp -s "${fixture_path}" "${CROSS_ARCH_FIXTURE}"; then
        echo "[ERROR] Deterministic fixtures differ byte-for-byte" >&2
        cmp -l "${fixture_path}" "${CROSS_ARCH_FIXTURE}" | head -20 >&2 || true
        exit 6
    fi
    identical_bytes=true
    cross_fixture_sha256="$(sha256sum "${CROSS_ARCH_FIXTURE}" | awk '{print $1}')"
fi

{
    echo "label=${LABEL}"
    echo "architecture=$(uname -m)"
    uname -a
    echo "protocol_fixture=$(file -b "${FIXTURE_TOOL}")"
    echo "fixture=$(file -b "${fixture_path}")"
    echo "frames=${FRAMES}"
    echo "fixture_bytes=$(stat -c %s "${fixture_path}")"
} > "${OUTPUT_DIR}/evidence/${LABEL}_platform.txt"

local_fixture_sha256="$(sha256sum "${fixture_path}" | awk '{print $1}')"
python3 - \
    "${OUTPUT_DIR}/evidence/acceptance_manifest.json" \
    "${LABEL}" \
    "$(uname -m)" \
    "${FRAMES}" \
    "${fixture_path}" \
    "${local_fixture_sha256}" \
    "${cross_verified}" \
    "${identical_bytes}" \
    "${CROSS_ARCH_FIXTURE}" \
    "${cross_fixture_sha256}" <<'PY'
import json
import sys
from pathlib import Path

(
    manifest_path,
    label,
    architecture,
    frames,
    fixture_path,
    fixture_sha256,
    cross_verified,
    identical_bytes,
    cross_fixture_path,
    cross_fixture_sha256,
) = sys.argv[1:]

manifest = {
    "label": label,
    "architecture": architecture,
    "protocol_version": 3,
    "wire_header_size": 44,
    "frames": int(frames),
    "fixture_path": fixture_path,
    "fixture_sha256": fixture_sha256,
    "self_verified": True,
    "cross_fixture_verified": cross_verified == "true",
    "byte_identical_to_cross_fixture": identical_bytes == "true",
    "cross_fixture_path": cross_fixture_path,
    "cross_fixture_sha256": cross_fixture_sha256,
}
Path(manifest_path).write_text(json.dumps(manifest, indent=2) + "\n")
PY

echo "[PASS] protocol fixture acceptance"
echo "[RESULT] label=${LABEL} arch=$(uname -m) frames=${FRAMES} sha256=${local_fixture_sha256}"
echo "[RESULT] cross_verified=${cross_verified} identical_bytes=${identical_bytes}"
echo "[INFO] Evidence: ${OUTPUT_DIR}/evidence"
