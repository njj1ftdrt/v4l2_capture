#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${MODE:-synthetic}"
SSH_TARGET="${SSH_TARGET:-}"
SSH_KEY="${SSH_KEY:-}"
TCP_TARGET_HOST="${TCP_TARGET_HOST:-}"
SSH_PORT="${SSH_PORT:-22}"
REMOTE_ROOT="${REMOTE_ROOT:-}"
REMOTE_BUILD_DIR_NAME="${REMOTE_BUILD_DIR_NAME:-build-arm64-cloud}"
REMOTE_INSTALL_DEPS="${REMOTE_INSTALL_DEPS:-1}"
PORT="${PORT:-19000}"
FRAMES="${FRAMES:-300}"
WIDTH="${WIDTH:-64}"
HEIGHT="${HEIGHT:-48}"
INTERVAL_MS="${INTERVAL_MS:-5}"
DEVICE="${DEVICE:-/dev/video0}"
CAMERA_WIDTH="${CAMERA_WIDTH:-640}"
CAMERA_HEIGHT="${CAMERA_HEIGHT:-360}"
TCP_QUEUE_CAPACITY="${TCP_QUEUE_CAPACITY:-8}"
LOCAL_BUILD_DIR="${LOCAL_BUILD_DIR:-${ROOT_DIR}/build}"
REMOTE_TIMEOUT_SECONDS="${REMOTE_TIMEOUT_SECONDS:-900}"
ALLOW_DIRTY="${ALLOW_DIRTY:-0}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/output/cross-machine-cloud-${MODE}}"

usage() {
    cat <<'USAGE'
Synthetic cross-machine acceptance:
  SSH_TARGET=ubuntu@ARM_PUBLIC_IP SSH_KEY=~/key.pem \
  MODE=synthetic FRAMES=300 PORT=19000 \
    ./scripts/cross_machine_cloud_acceptance.sh

Real-camera cross-machine acceptance:
  SSH_TARGET=ubuntu@ARM_PUBLIC_IP SSH_KEY=~/key.pem \
  MODE=camera DEVICE=/dev/video0 FRAMES=300 PORT=19000 \
    ./scripts/cross_machine_cloud_acceptance.sh
USAGE
}

if [[ -z "${SSH_TARGET}" ]]; then
    usage >&2
    exit 2
fi
if [[ -z "${TCP_TARGET_HOST}" ]]; then
    TCP_TARGET_HOST="${TCP_TARGET_HOST}"
fi
if [[ "${MODE}" != "synthetic" && "${MODE}" != "camera" ]]; then
    echo "[ERROR] MODE must be synthetic or camera" >&2
    exit 2
fi
if [[ ! "${PORT}" =~ ^[0-9]+$ ]] || (( PORT < 1 || PORT > 65535 )); then
    echo "[ERROR] PORT must be between 1 and 65535" >&2
    exit 2
fi
if [[ ! "${FRAMES}" =~ ^[0-9]+$ ]] || (( FRAMES < 1 )); then
    echo "[ERROR] FRAMES must be positive" >&2
    exit 2
fi

required=(git ssh tar python3 sha256sum file)
for cmd in "${required[@]}"; do
    command -v "${cmd}" >/dev/null 2>&1 || {
        echo "[ERROR] Missing local command: ${cmd}" >&2
        exit 3
    }
done

SSH_ARGS=(-p "${SSH_PORT}" -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=15)
if [[ -n "${SSH_KEY}" ]]; then
    [[ -f "${SSH_KEY}" ]] || { echo "[ERROR] SSH key not found: ${SSH_KEY}" >&2; exit 3; }
    chmod 600 "${SSH_KEY}"
    SSH_ARGS+=(-i "${SSH_KEY}")
fi
cd "${ROOT_DIR}"
commit="$(git rev-parse HEAD)"
dirty="$(git status --porcelain)"
if [[ -n "${dirty}" && "${ALLOW_DIRTY}" != "1" ]]; then
    echo "[ERROR] Working tree is not clean. Commit changes first or set ALLOW_DIRTY=1." >&2
    git status --short >&2
    exit 4
fi

remote_arch="$(ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" 'uname -m')"
case "${remote_arch}" in
    aarch64|arm64) ;;
    *) echo "[ERROR] Remote host is not ARM64: ${remote_arch}" >&2; exit 5 ;;
esac

remote_home="$(ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" 'printf %s "$HOME"')"
if [[ -z "${REMOTE_ROOT}" ]]; then
    REMOTE_ROOT="${remote_home}/v4l2_capture_cross_machine"
fi
if [[ "${REMOTE_ROOT}" == *" "* ]]; then
    echo "[ERROR] REMOTE_ROOT must not contain spaces" >&2
    exit 5
fi
remote_build="${REMOTE_ROOT}/${REMOTE_BUILD_DIR_NAME}"
remote_prepare_output="${REMOTE_ROOT}/output/arm64-cloud-prepare"
remote_receiver_output="${REMOTE_ROOT}/output/cloud-receiver"

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}/evidence" "${OUTPUT_DIR}/remote"

{
    echo "mode=${MODE}"
    echo "architecture=$(uname -m)"
    echo "hostname=$(hostname)"
    echo "source_commit=${commit}"
    echo "target=${SSH_TARGET}"
    echo "port=${PORT}"
    echo "frames=${FRAMES}"
    echo "utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    uname -a
    ip route get "${SSH_TARGET#*@}" 2>/dev/null || true
} > "${OUTPUT_DIR}/evidence/local_platform.txt"

cleanup_remote() {
    set +e
    if [[ -n "${remote_pid:-}" ]]; then
        ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" \
            "kill -INT '${remote_pid}' 2>/dev/null || true" >/dev/null 2>&1 || true
    fi
}
trap cleanup_remote EXIT INT TERM

echo "[INFO] Uploading exact Git commit ${commit} to ${SSH_TARGET}:${REMOTE_ROOT}"
git archive --format=tar HEAD | \
    ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" \
        "rm -rf '${REMOTE_ROOT}' && mkdir -p '${REMOTE_ROOT}' && tar -xf - -C '${REMOTE_ROOT}'"

echo "[INFO] Building and testing natively on ARM64"
ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" \
    "cd '${REMOTE_ROOT}' && SOURCE_COMMIT='${commit}' INSTALL_DEPS='${REMOTE_INSTALL_DEPS}' BUILD_DIR='${remote_build}' OUTPUT_DIR='${remote_prepare_output}' ./scripts/arm64_cloud_prepare.sh"

remote_max_frames="${FRAMES}"
if [[ "${MODE}" == "camera" ]]; then
    remote_max_frames=0
fi

remote_pid="$(ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" bash -s -- \
    "${remote_build}/tcp_receiver" "${remote_receiver_output}" "${PORT}" "${remote_max_frames}" <<'REMOTE'
set -euo pipefail
receiver="$1"
out="$2"
port="$3"
max_frames="$4"
rm -rf "$out"
mkdir -p "$out"
cat > "$out/run_receiver.sh" <<EOF_RUN
#!/usr/bin/env bash
set +e
"$receiver" \\
  --port "$port" \\
  --max-frames "$max_frames" \\
  --max-sessions 1 \\
  --discard-payload \\
  --output "$out/frames" \\
  --stats-output "$out/receiver_stats.json" \\
  >"$out/receiver.log" 2>&1
rc=\$?
echo "\$rc" > "$out/receiver.exit_code"
exit "\$rc"
EOF_RUN
chmod +x "$out/run_receiver.sh"
nohup "$out/run_receiver.sh" >"$out/launcher.log" 2>&1 < /dev/null &
echo $!
REMOTE
)"
echo "[INFO] Remote receiver PID=${remote_pid}"

ready=0
for _ in $(seq 1 60); do
    if ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" \
        "ss -ltnH | awk '{print \$4}' | grep -Eq '[:.]${PORT}$'"; then
        ready=1
        break
    fi
    if ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" \
        "test -f '${remote_receiver_output}/receiver.exit_code'"; then
        break
    fi
    sleep 1
done
if [[ "${ready}" != "1" ]]; then
    echo "[ERROR] Remote receiver did not listen on port ${PORT}" >&2
    ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" \
        "cat '${remote_receiver_output}/receiver.log' 2>/dev/null || true" >&2
    exit 6
fi

if [[ "${MODE}" == "synthetic" ]]; then
    BUILD_DIR="${LOCAL_BUILD_DIR}" \
    TARGET_HOST="${TCP_TARGET_HOST}" \
    PORT="${PORT}" \
    FRAMES="${FRAMES}" \
    WIDTH="${WIDTH}" \
    HEIGHT="${HEIGHT}" \
    INTERVAL_MS="${INTERVAL_MS}" \
    OUTPUT_DIR="${OUTPUT_DIR}/sender" \
        ./scripts/cross_machine_tcp_acceptance.sh sender
else
    capture="${LOCAL_BUILD_DIR}/v4l2_capture"
    [[ -x "${capture}" ]] || { echo "[ERROR] Missing local capture binary: ${capture}" >&2; exit 7; }
    [[ -e "${DEVICE}" ]] || { echo "[ERROR] Camera device missing: ${DEVICE}" >&2; exit 7; }
    mkdir -p "${OUTPUT_DIR}/sender/evidence"
    file -b "${capture}" > "${OUTPUT_DIR}/sender/evidence/binary.txt"
    (cd "${LOCAL_BUILD_DIR}" && sha256sum v4l2_capture) > "${OUTPUT_DIR}/sender/evidence/binary.sha256"
    "${capture}" \
        --device "${DEVICE}" \
        --width "${CAMERA_WIDTH}" \
        --height "${CAMERA_HEIGHT}" \
        --format YUYV \
        --mmap-buffers 4 \
        --pipeline-frames "${FRAMES}" \
        --ring-capacity 8 \
        --tcp-host "${TCP_TARGET_HOST}" \
        --tcp-port "${PORT}" \
        --tcp-queue-capacity "${TCP_QUEUE_CAPACITY}" \
        --tcp-connect-max-attempts 20 \
        --tcp-connect-retry-delay-ms 500 \
        --tcp-send-timeout-ms 5000 \
        --timeout-ms 1000 \
        --camera-timeout-recovery-threshold 3 \
        --camera-invalid-frame-recovery-threshold 30 \
        --camera-invalid-frame-recovery-cooldown-frames 300 \
        --camera-invalid-frame-recovery-max-count 3 \
        --camera-recovery-max-attempts 60 \
        --camera-recovery-retry-delay-ms 1000 \
        --stats-output "${OUTPUT_DIR}/sender/pipeline_stats.json" \
        >"${OUTPUT_DIR}/sender/camera.log" 2>&1
fi

elapsed=0
while ! ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" \
    "test -f '${remote_receiver_output}/receiver.exit_code'"; do
    if (( elapsed >= REMOTE_TIMEOUT_SECONDS )); then
        echo "[ERROR] Timed out waiting for remote receiver" >&2
        exit 8
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done
remote_rc="$(ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" \
    "cat '${remote_receiver_output}/receiver.exit_code'")"
unset remote_pid
if [[ "${remote_rc}" != "0" ]]; then
    echo "[ERROR] Remote receiver exited with ${remote_rc}" >&2
    ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" \
        "tail -100 '${remote_receiver_output}/receiver.log'" >&2 || true
    exit 9
fi

ssh "${SSH_ARGS[@]}" "${SSH_TARGET}" \
    "tar -C '${REMOTE_ROOT}/output' -czf - arm64-cloud-prepare cloud-receiver" | \
    tar -xzf - -C "${OUTPUT_DIR}/remote"

python3 - \
    "${MODE}" "${FRAMES}" "${commit}" "${remote_arch}" "${PORT}" \
    "${OUTPUT_DIR}" "${SSH_TARGET#*@}" <<'PY'
import json
import sys
from pathlib import Path

mode, expected_text, commit, remote_arch, port_text, out_text, host = sys.argv[1:]
expected = int(expected_text)
port = int(port_text)
out = Path(out_text)
receiver = json.loads((out / "remote/cloud-receiver/receiver_stats.json").read_text())

failures = []
for key in ("crc_errors", "header_errors", "rejected_frames"):
    if int(receiver.get(key, -1)) != 0:
        failures.append(f"receiver {key}={receiver.get(key)}")
if int(receiver.get("accepted_sessions", -1)) != 1:
    failures.append(f"accepted_sessions={receiver.get('accepted_sessions')}")
if int(receiver.get("completed_sessions", -1)) != 1:
    failures.append(f"completed_sessions={receiver.get('completed_sessions')}")
if int(receiver.get("saved_files", -1)) != 0:
    failures.append(f"saved_files={receiver.get('saved_files')} expected=0")

manifest = {
    "passed": False,
    "mode": mode,
    "source_commit": commit,
    "local_architecture": __import__('platform').machine(),
    "remote_architecture": remote_arch,
    "target_host": host,
    "port": port,
    "receiver": receiver,
}

if mode == "synthetic":
    sender = json.loads((out / "sender/evidence/acceptance_manifest.json").read_text())
    sent = int(sender.get("sent_frames", -1))
    received = int(receiver.get("received_frames", -1))
    if sent != expected:
        failures.append(f"sender sent_frames={sent} expected={expected}")
    if received != expected:
        failures.append(f"receiver received_frames={received} expected={expected}")
    manifest["sender"] = sender
else:
    sender = json.loads((out / "sender/pipeline_stats.json").read_text())
    produced = int(sender.get("produced_frames", -1))
    consumed = int(sender.get("consumed_frames", -1))
    dropped = int(sender.get("ring_dropped_frames", -1))
    invalid = int(sender.get("invalid_frames", -1))
    queued = int(sender.get("tcp_queued_frames", -1))
    sent = int(sender.get("tcp_sent_frames", -1))
    received = int(receiver.get("received_frames", -1))
    if produced != consumed + dropped:
        failures.append("camera produced != consumed + ring_dropped")
    if consumed != invalid + queued:
        failures.append("camera consumed != invalid + tcp_queued")
    if queued != sent:
        failures.append("camera tcp_queued != tcp_sent")
    if sent != received:
        failures.append(f"cross-machine sent={sent} received={received}")
    for key in ("tcp_send_errors", "tcp_send_timeouts", "tcp_send_cancellations"):
        if int(sender.get(key, -1)) != 0:
            failures.append(f"camera {key}={sender.get(key)}")
    manifest["sender"] = sender

if failures:
    manifest["failures"] = failures
    (out / "evidence/combined_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    raise SystemExit("; ".join(failures))

manifest["passed"] = True
(out / "evidence/combined_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(f"[PASS] {mode} cross-machine acceptance: sent={sent} received={received}")
PY

(
    cd "${OUTPUT_DIR}"
    find . -type f -print0 | sort -z | xargs -0 sha256sum > evidence/all_files.sha256
)
archive="${OUTPUT_DIR}.tar.gz"
tar -C "$(dirname "${OUTPUT_DIR}")" -czf "${archive}" "$(basename "${OUTPUT_DIR}")"
(
    cd "$(dirname "${archive}")"
    sha256sum "$(basename "${archive}")" > "$(basename "${archive}").sha256"
)

echo "[PASS] Cross-machine cloud acceptance completed"
echo "[INFO] Evidence directory: ${OUTPUT_DIR}"
echo "[INFO] Evidence archive: ${archive}"
