# v4l2_ros2_adapter

This optional ROS 2 Jazzy adapter keeps ROS 2 out of the original V4L2/TCP core. It listens for protocol version 3 TCP frames, validates headers before payload allocation, verifies CRC32, measures host-side end-to-end latency, and publishes standard ROS 2 camera messages.

## Topics

```text
/camera/image_raw             sensor_msgs/msg/Image
/camera/camera_info           sensor_msgs/msg/CameraInfo
/camera_link/diagnostics      diagnostic_msgs/msg/DiagnosticArray
```

The default image encoding is `rgb8`. The adapter converts the incoming packed YUYV payload to RGB8 so RViz2 and common vision consumers can display it directly. Set `output_encoding: yuv422_yuy2` to publish the original packed bytes without color conversion.

Image and CameraInfo messages use the original host-side capture timestamp from protocol v3 and the same `camera_frame_id`. CameraInfo is intentionally uncalibrated while `camera_fx` and `camera_fy` remain zero; this follows the standard convention that `K[0] == 0` means no calibration is available. Fill the calibration parameters only with values measured for the actual camera and resolution.

## Build

```bash
source /opt/ros/jazzy/setup.bash
cd ~/v4l2_capture/ros2_ws
colcon build --symlink-install --packages-select v4l2_ros2_adapter
source install/setup.bash
```

## Run

```bash
ros2 launch v4l2_ros2_adapter diagnostics.launch.py
```

The default TCP port is `9800`. Send deterministic test frames from the core project:

```bash
cd ~/v4l2_capture
./build/tcp_sender \
  --host 127.0.0.1 \
  --port 9800 \
  --frames 30 \
  --width 640 \
  --height 360 \
  --format YUYV \
  --interval-ms 33
```

For a real USB camera, start the adapter first and then run the core pipeline with `--tcp-port 9800`.

## Diagnostics

The diagnostic status contains connection state, rolling receive FPS, frame and byte counts, sequential session counters, header/CRC rejection counts, clock-order errors, image publication counts, frame metadata, calibration state, and E2E latency mean/P50/P95/P99/max/jitter values.

`max_frames=0` and `max_sessions=0` mean unlimited service-style operation. A malformed header closes only the current client session because its untrusted payload length is not consumed.

## Validated DDS profile

The validated Stage 12C profile is Cyclone DDS:

```bash
export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
```

On the development host, default Fast DDS discovery and small CameraInfo delivery succeeded, but 640x360 RGB8 Image messages did not reach the subscriber until the RMW implementation was switched. This is documented as an environment-specific default-configuration compatibility issue, not a general limitation of Fast DDS.

Image QoS can be configured with `image_qos_reliability` (`best_effort` or `reliable`) and `image_qos_depth`.

Run the automated integration and rosbag2 checks from the project root:

```bash
./scripts/ros2_camera_integration_test.sh
./scripts/ros2_camera_rosbag_test.sh
```

Start the adapter or adapter plus RViz2 through the validated wrappers:

```bash
./scripts/run_ros2_camera_adapter.sh
./scripts/run_ros2_camera_rviz.sh
```
