# v4l2_ros2_adapter

Stage 12A adds an optional ROS 2 Jazzy diagnostics adapter without linking ROS 2 into the original V4L2 capture executable.

The node listens for protocol version 3 TCP frames, performs the same defensive header validation and CRC32 verification as `tcp_receiver`, measures host-side end-to-end latency, and publishes `diagnostic_msgs/msg/DiagnosticArray` on:

```text
/camera_link/diagnostics
```

This stage does not publish image data yet. `sensor_msgs/Image` and `sensor_msgs/CameraInfo` are added in Stage 12B.

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

Inspect diagnostics:

```bash
source /opt/ros/jazzy/setup.bash
source ~/v4l2_capture/ros2_ws/install/setup.bash
ros2 topic echo /camera_link/diagnostics diagnostic_msgs/msg/DiagnosticArray
```

## Published values

The diagnostic status contains connection state, rolling receive FPS, frame and byte counts, sequential session counters, header/CRC rejection counts, clock-order errors, last frame metadata, and E2E latency mean/P50/P95/P99/max/jitter values.

`max_frames=0` and `max_sessions=0` mean unlimited service-style operation. A malformed header closes only the current client session because its untrusted payload length is not consumed.
