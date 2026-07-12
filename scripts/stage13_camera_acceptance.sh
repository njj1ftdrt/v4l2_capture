#!/usr/bin/env bash
set -eo pipefail

PROJECT_ROOT="$HOME/v4l2_capture"

RUN_LABEL="${RUN_LABEL:-stage13a}"
FRAMES="${FRAMES:-900}"
PORT="${PORT:-9813}"
WARMUP_SECONDS="${WARMUP_SECONDS:-3}"
BENCHMARK_SECONDS="${BENCHMARK_SECONDS:-20}"
OUTPUT_ENCODING="${OUTPUT_ENCODING:-rgb8}"

ros_double()
{
    local value="$1"

    case "${value}" in
        *.*|*e*|*E*)
            printf '%s' "${value}"
            ;;
        *)
            printf '%s.0' "${value}"
            ;;
    esac
}

WARMUP_SECONDS_ROS="$(
    ros_double "${WARMUP_SECONDS}"
)"

BENCHMARK_SECONDS_ROS="$(
    ros_double "${BENCHMARK_SECONDS}"
)"


LOG_DIR="${PROJECT_ROOT}/docs/logs/${RUN_LABEL}"
OUTPUT_DIR="${PROJECT_ROOT}/output/${RUN_LABEL}"

PIPELINE_JSON="${OUTPUT_DIR}/pipeline_stats.json"
BENCHMARK_JSON="${OUTPUT_DIR}/cpp_image_benchmark.json"

ADAPTER_LOG="${LOG_DIR}/adapter.txt"
CAMERA_LOG="${LOG_DIR}/camera.txt"
CPP_LOG="${LOG_DIR}/cpp_image_benchmark.txt"
DIAGNOSTICS_LOG="${LOG_DIR}/diagnostics_final.txt"
RESOURCE_CSV="${LOG_DIR}/resource_samples.csv"
CAMERA_TIME_LOG="${LOG_DIR}/camera_time.txt"

ADAPTER_PGID=""
CAMERA_PGID=""
SAMPLER_PID=""

source /opt/ros/jazzy/setup.bash
source "${PROJECT_ROOT}/ros2_ws/install/setup.bash"

export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp

group_alive()
{
    local pgid="$1"

    [[ -n "${pgid}" ]] &&
        kill -0 -- "-${pgid}" 2>/dev/null
}

stop_group()
{
    local pgid="$1"
    local label="$2"

    [[ -z "${pgid}" ]] && return 0

    if ! group_alive "${pgid}"; then
        wait "${pgid}" 2>/dev/null || true
        return 0
    fi

    echo "[CLEANUP] stopping ${label}"

    kill -INT -- "-${pgid}" 2>/dev/null || true

    for _ in $(seq 1 30); do
        group_alive "${pgid}" || break
        sleep 0.1
    done

    if group_alive "${pgid}"; then
        kill -TERM -- "-${pgid}" 2>/dev/null || true
    fi

    for _ in $(seq 1 30); do
        group_alive "${pgid}" || break
        sleep 0.1
    done

    if group_alive "${pgid}"; then
        kill -KILL -- "-${pgid}" 2>/dev/null || true
    fi

    wait "${pgid}" 2>/dev/null || true
}

cleanup()
{
    set +e

    if [[ -n "${SAMPLER_PID}" ]]; then
        kill -TERM "${SAMPLER_PID}" 2>/dev/null || true
        wait "${SAMPLER_PID}" 2>/dev/null || true
    fi

    stop_group "${CAMERA_PGID}" "camera pipeline"
    stop_group "${ADAPTER_PGID}" "ROS2 adapter"
}

trap cleanup EXIT INT TERM

mkdir -p "${LOG_DIR}" "${OUTPUT_DIR}"

rm -f \
    "${PIPELINE_JSON}" \
    "${BENCHMARK_JSON}" \
    "${ADAPTER_LOG}" \
    "${CAMERA_LOG}" \
    "${CPP_LOG}" \
    "${DIAGNOSTICS_LOG}" \
    "${RESOURCE_CSV}" \
    "${CAMERA_TIME_LOG}"

echo "========== ${RUN_LABEL} =========="
echo "RMW                   : ${RMW_IMPLEMENTATION}"
echo "frames                : ${FRAMES}"
echo "port                  : ${PORT}"
echo "warmup seconds        : ${WARMUP_SECONDS}"
echo "benchmark seconds     : ${BENCHMARK_SECONDS}"
echo "output encoding       : ${OUTPUT_ENCODING}"
echo "=================================="

pkill -TERM -f v4l2_diagnostics_node \
    2>/dev/null || true

pkill -TERM -f diagnostics.launch.py \
    2>/dev/null || true

pkill -TERM -x v4l2_capture \
    2>/dev/null || true

sleep 2

setsid ros2 launch \
    v4l2_ros2_adapter \
    diagnostics.launch.py \
    listen_port:="${PORT}" \
    output_encoding:="${OUTPUT_ENCODING}" \
    image_qos_depth:=1 \
    > "${ADAPTER_LOG}" 2>&1 &

ADAPTER_PGID=$!

sleep 3

if ! group_alive "${ADAPTER_PGID}"; then
    echo "[FAIL] ROS2 adapter exited"
    cat "${ADAPTER_LOG}"
    exit 1
fi

cd "${PROJECT_ROOT}"

setsid /usr/bin/time \
    -v \
    -o "${CAMERA_TIME_LOG}" \
    ./build/v4l2_capture \
    --device /dev/video0 \
    --width 640 \
    --height 360 \
    --format YUYV \
    --mmap-buffers 4 \
    --pipeline-frames "${FRAMES}" \
    --ring-capacity 8 \
    --tcp-host 127.0.0.1 \
    --tcp-port "${PORT}" \
    --tcp-queue-capacity 8 \
    --tcp-connect-max-attempts 5 \
    --tcp-connect-retry-delay-ms 500 \
    --stats-output "${PIPELINE_JSON}" \
    --timeout-ms 2000 \
    > "${CAMERA_LOG}" 2>&1 &

CAMERA_PGID=$!

echo \
"timestamp,camera_cpu_percent,camera_rss_kb,adapter_cpu_percent,adapter_rss_kb" \
    > "${RESOURCE_CSV}"

(
    while group_alive "${CAMERA_PGID}"; do
        timestamp="$(date +%s)"

        camera_pid="$(
            pgrep -n -x v4l2_capture \
                2>/dev/null || true
        )"

        adapter_pid="$(
            pgrep -n -f \
                '/v4l2_diagnostics_node' \
                2>/dev/null || true
        )"

        camera_cpu=""
        camera_rss=""
        adapter_cpu=""
        adapter_rss=""

        if [[ -n "${camera_pid}" ]]; then
            read -r camera_cpu camera_rss < <(
                ps -p "${camera_pid}" \
                    -o %cpu=,rss= \
                    2>/dev/null || true
            )
        fi

        if [[ -n "${adapter_pid}" ]]; then
            read -r adapter_cpu adapter_rss < <(
                ps -p "${adapter_pid}" \
                    -o %cpu=,rss= \
                    2>/dev/null || true
            )
        fi

        echo \
"${timestamp},${camera_cpu},${camera_rss},${adapter_cpu},${adapter_rss}" \
            >> "${RESOURCE_CSV}"

        sleep 2
    done
) &

SAMPLER_PID=$!

IMAGE_READY=0

for _ in $(seq 1 20); do
    if timeout 2 \
        ros2 topic echo \
        /camera/image_raw \
        sensor_msgs/msg/Image \
        --field header \
        --once \
        --qos-reliability best_effort \
        >/dev/null 2>&1
    then
        IMAGE_READY=1
        break
    fi

    sleep 1
done

if [[ "${IMAGE_READY}" -ne 1 ]]; then
    echo "[FAIL] /camera/image_raw did not become ready"
    tail -n 100 "${ADAPTER_LOG}" || true
    tail -n 100 "${CAMERA_LOG}" || true
    exit 1
fi

set +e

timeout \
    --signal=INT \
    --kill-after=5s \
    "$((WARMUP_SECONDS + BENCHMARK_SECONDS + 20))" \
    ros2 run \
    v4l2_ros2_adapter \
    image_throughput_check \
    --ros-args \
    -p topic:=/camera/image_raw \
    -p warmup_seconds:="${WARMUP_SECONDS_ROS}" \
    -p measurement_seconds:="${BENCHMARK_SECONDS_ROS}" \
    -p expected_width:=640 \
    -p expected_height:=360 \
    -p expected_encoding:="${OUTPUT_ENCODING}" \
    -p output_path:="${BENCHMARK_JSON}" \
    > "${CPP_LOG}" 2>&1

CPP_STATUS=$?

set -e

echo
cat "${CPP_LOG}"

set +e
wait "${CAMERA_PGID}"
CAMERA_STATUS=$?
set -e

CAMERA_PGID=""

if [[ -n "${SAMPLER_PID}" ]]; then
    kill -TERM "${SAMPLER_PID}" 2>/dev/null || true
    wait "${SAMPLER_PID}" 2>/dev/null || true
    SAMPLER_PID=""
fi

sleep 2

set +e

timeout 8 \
    ros2 topic echo \
    --once \
    /camera_link/diagnostics \
    diagnostic_msgs/msg/DiagnosticArray \
    > "${DIAGNOSTICS_LOG}" 2>&1

DIAGNOSTICS_STATUS=$?

set -e

stop_group "${ADAPTER_PGID}" "ROS2 adapter"
ADAPTER_PGID=""

trap - EXIT INT TERM

python3 - \
    "${PIPELINE_JSON}" \
    "${BENCHMARK_JSON}" \
    "${RESOURCE_CSV}" \
    "${DIAGNOSTICS_LOG}" \
    "${CPP_STATUS}" \
    "${CAMERA_STATUS}" \
    "${DIAGNOSTICS_STATUS}" <<'PY'
import csv
import json
import re
import statistics
import sys
from pathlib import Path

(
    pipeline_path,
    benchmark_path,
    resource_path,
    diagnostics_path,
    cpp_status,
    camera_status,
    diagnostics_status,
) = sys.argv[1:]

errors = []

if int(cpp_status) != 0:
    errors.append(
        f"C++ benchmark exit code is {cpp_status}"
    )

if int(camera_status) != 0:
    errors.append(
        f"camera pipeline exit code is {camera_status}"
    )

if int(diagnostics_status) != 0:
    errors.append(
        "failed to capture final diagnostics"
    )

pipeline_file = Path(pipeline_path)
benchmark_file = Path(benchmark_path)

if not pipeline_file.exists():
    errors.append("pipeline_stats.json is missing")
    pipeline = {}
else:
    pipeline = json.loads(
        pipeline_file.read_text(encoding="utf-8")
    )

if not benchmark_file.exists():
    errors.append("C++ benchmark JSON is missing")
    benchmark = {}
else:
    benchmark = json.loads(
        benchmark_file.read_text(encoding="utf-8")
    )

produced = int(pipeline.get("produced_frames", 0))
consumed = int(pipeline.get("consumed_frames", 0))
invalid = int(pipeline.get("invalid_frames", 0))
sent = int(pipeline.get("tcp_sent_frames", 0))

if produced != consumed:
    errors.append(
        "produced_frames != consumed_frames"
    )

if sent + invalid != consumed:
    errors.append(
        "tcp_sent_frames + invalid_frames "
        "!= consumed_frames"
    )

for key in [
    "ring_dropped_frames",
    "tcp_queue_dropped",
    "tcp_send_errors",
]:
    if int(pipeline.get(key, 0)) != 0:
        errors.append(f"{key} != 0")

if int(benchmark.get("invalid_images", 0)) != 0:
    errors.append(
        "C++ subscriber detected invalid Images"
    )

producer_fps = float(
    pipeline.get("producer_fps", 0.0)
)

cpp_rate = float(
    benchmark.get("receive_rate_hz", 0.0)
)

ratio = (
    cpp_rate / producer_fps
    if producer_fps > 0.0
    else 0.0
)

diagnostics_text = Path(
    diagnostics_path
).read_text(
    encoding="utf-8",
    errors="replace",
) if Path(diagnostics_path).exists() else ""

def diagnostic_value(name):
    pattern = (
        rf"key:\s*{re.escape(name)}\s*\n"
        rf"\s*value:\s*'?([^'\n]+)'?"
    )

    match = re.search(pattern, diagnostics_text)

    return match.group(1).strip() if match else None

received_frames = diagnostic_value(
    "received_frames"
)

published_images = diagnostic_value(
    "published_images"
)

for name in [
    "header_errors",
    "crc_errors",
    "rejected_frames",
    "image_publish_errors",
    "latency_clock_errors",
]:
    value = diagnostic_value(name)

    if value is not None and int(value) != 0:
        errors.append(f"{name} != 0")

if received_frames is not None:
    if int(received_frames) != sent:
        errors.append(
            "final ROS2 received_frames "
            "!= tcp_sent_frames"
        )

if (
    received_frames is not None
    and published_images is not None
    and int(received_frames) != int(published_images)
):
    errors.append(
        "received_frames != published_images"
    )

camera_cpu = []
camera_rss = []
adapter_cpu = []
adapter_rss = []

resource_file = Path(resource_path)

if resource_file.exists():
    with resource_file.open(
        "r",
        encoding="utf-8",
        newline="",
    ) as file:
        for row in csv.DictReader(file):
            for source, target in [
                ("camera_cpu_percent", camera_cpu),
                ("camera_rss_kb", camera_rss),
                ("adapter_cpu_percent", adapter_cpu),
                ("adapter_rss_kb", adapter_rss),
            ]:
                value = row.get(source, "").strip()

                if value:
                    try:
                        target.append(float(value))
                    except ValueError:
                        pass

def mean_or_zero(values):
    return (
        statistics.mean(values)
        if values
        else 0.0
    )

def max_or_zero(values):
    return max(values) if values else 0.0

print()
print("========== Stage 13 Summary ==========")
print(f"produced frames          : {produced}")
print(f"consumed frames          : {consumed}")
print(f"tcp sent frames          : {sent}")
print(f"invalid frames           : {invalid}")
print(
    f"producer FPS             : "
    f"{producer_fps:.3f}"
)
print(
    f"C++ subscriber rate      : "
    f"{cpp_rate:.3f} Hz"
)
print(
    f"C++ / producer ratio     : "
    f"{ratio * 100.0:.1f}%"
)
print(
    f"ROS2 received frames     : "
    f"{received_frames}"
)
print(
    f"ROS2 published images    : "
    f"{published_images}"
)
print(
    f"camera CPU mean/max      : "
    f"{mean_or_zero(camera_cpu):.1f}% / "
    f"{max_or_zero(camera_cpu):.1f}%"
)
print(
    f"camera RSS mean/max      : "
    f"{mean_or_zero(camera_rss):.0f} / "
    f"{max_or_zero(camera_rss):.0f} KiB"
)
print(
    f"adapter CPU mean/max     : "
    f"{mean_or_zero(adapter_cpu):.1f}% / "
    f"{max_or_zero(adapter_cpu):.1f}%"
)
print(
    f"adapter RSS mean/max     : "
    f"{mean_or_zero(adapter_rss):.0f} / "
    f"{max_or_zero(adapter_rss):.0f} KiB"
)

if not benchmark:
    print(
        "performance assessment  : "
        "NOT MEASURED"
    )
elif not benchmark:
    print(
        "performance assessment  : "
        "NOT MEASURED"
    )
elif not benchmark:
    print(
        "performance assessment  : "
        "NOT MEASURED"
    )
elif ratio >= 0.85:
    print(
        "performance assessment  : "
        "TARGET MET"
    )
elif ratio >= 0.65:
    print(
        "performance assessment  : "
        "USABLE, OPTIMIZATION RECOMMENDED"
    )
else:
    print(
        "performance assessment  : "
        "DOWNSTREAM BOTTLENECK CONFIRMED"
    )

if errors:
    for error in errors:
        print(f"[FAIL] {error}")

    raise SystemExit(1)

print("[PASS] pipeline and ROS2 invariants satisfied")
print("======================================")
PY

echo
echo "[PASS] ${RUN_LABEL} completed"
