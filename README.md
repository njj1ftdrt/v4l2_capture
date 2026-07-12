# Linux V4L2 Camera Capture and TCP Frame Transport

## Project Overview

This project is an embedded Linux camera acquisition and image frame transport system.

It directly uses the Linux V4L2 API instead of OpenCV to capture raw frames from a USB camera. The system uses a multi-threaded pipeline, RingBuffer-based decoupling, and a custom TCP FrameHeader + Payload protocol to transmit image frames to a receiver.

The project is designed for embedded Linux, robotics, smart hardware, and camera data pipeline development scenarios.

## Target Roles

This project is suitable for demonstrating skills required by:

- Embedded Linux software development
- Robotics software development
- Smart hardware software development
- Linux C/C++ development
- Camera acquisition and video transport
- Autonomous driving perception data pipeline prototype

## System Architecture

```text
USB Camera
  ↓
Linux V4L2
open / ioctl / mmap / poll / DQBUF / QBUF
  ↓
Frame
  ↓
Main RingBuffer
  ↓
Consumer Thread
  ↓
TCP RingBuffer
  ↓
Dedicated TCP Sender Thread
  ↓
TCP Socket
FrameHeader + Payload
  ↓
TCP Receiver
  ↓
YUYV File Reconstruction
Device : /dev/video0
Format : YUYV
Size   : 640x360
FPS    : 30 fps target
640 * 360 * 2 = 460800 bytes
producer thread
  → capture frames from V4L2
  → push frames into RingBuffer

consumer thread
  → pop frames from RingBuffer
  → validate frames
  → save locally or enqueue to TCP queue
FrameHeader + Payload
consumer thread
  → enqueue valid frames to tcp_ring

tcp_sender thread
  → pop frames from tcp_ring
  → send FrameHeader + Payload
./scripts/tcp_v4l2_regression_test.sh
cmake -S . -B build
cmake --build build -j
./build/test_ring_buffer
RingBuffer tests passed.
./build/tcp_receiver \
  --port 9000 \
  --output output/tcp_recv \
  --max-frames 1
./build/tcp_sender \
  --host 127.0.0.1 \
  --port 9000 \
  --frames 1 \
  --width 640 \
  --height 360 \
  --format YUYV
stat -c '%n %s bytes' output/tcp_recv/*.YUYV
460800 bytes
./build/tcp_receiver \
  --port 9000 \
  --output output/tcp_recv_v4l2_300 \
  --max-frames 300
./build/v4l2_capture \
  --device /dev/video0 \
  --width 640 \
  --height 360 \
  --format YUYV \
  --mmap-buffers 4 \
  --pipeline-frames 300 \
  --ring-capacity 8 \
  --tcp-host 127.0.0.1 \
  --tcp-port 9000 \
  --tcp-queue-capacity 8 \
  --timeout-ms 2000
find output/tcp_recv_v4l2_300 -type f -name '*.YUYV' | wc -l
stat -c '%s' output/tcp_recv_v4l2_300/*.YUYV | sort | uniq -c
300
    300 460800
./scripts/tcp_v4l2_regression_test.sh
[PASS] TCP V4L2 regression test passed.
produced frames      : 300
consumed frames      : 300
ring dropped frames  : 0
tcp sent frames      : 300
tcp sent bytes       : 138252000
invalid frames       : 0
consumed bytes       : 138240000
300 YUYV files
each file size = 460800 bytes

## Machine-Readable Statistics

`v4l2_capture` can export sender-side JSON statistics:

```bash
./build/v4l2_capture \
  --config config/v4l2_tcp_pipeline.conf \
  --stats-output output/stats/pipeline_stats.json
```

`tcp_receiver` can export receiver-side JSON statistics:

```bash
./build/tcp_receiver \
  --port 9000 \
  --output output/tcp_recv \
  --max-frames 300 \
  --stats-output output/stats/receiver_stats.json
```

The automated regression script uses these JSON files instead of parsing Logger text output.

## Defensive TCP Header Validation

The TCP receiver validates protocol metadata before allocating memory for a frame payload. The default maximum accepted payload is 16 MiB:

```bash
./build/tcp_receiver \
  --port 9000 \
  --output output/tcp_recv \
  --stats-output output/stats/receiver_stats.json \
  --max-payload-bytes 16777216
```

Run protocol validation tests:

```bash
ctest --test-dir build --output-on-failure
./scripts/tcp_protocol_negative_test.sh
```

## Sequential Receiver Sessions

`tcp_receiver` can keep its listening socket open and accept multiple clients sequentially:

```bash
./build/tcp_receiver \
  --port 9500 \
  --output output/multisession/recv \
  --max-frames 5 \
  --max-sessions 2 \
  --stats-output output/multisession/receiver_stats.json
```

The default remains `--max-sessions 1`, preserving the original single-client behavior. Session counters are exported in receiver JSON statistics.

## Bounded TCP Connection Retry

The V4L2 pipeline supports bounded retry when the receiver is not yet listening:

```bash
./build/v4l2_capture \
  --config config/v4l2_tcp_pipeline.conf \
  --tcp-connect-max-attempts 5 \
  --tcp-connect-retry-delay-ms 500
```

The retry policy is applied before the application producer and consumer threads begin processing frames. This prevents the TCP RingBuffer from filling while the receiver is unavailable.

This policy covers initial connection establishment only. A failure after part of a frame has already been sent is reported and the pipeline stops; it does not blindly retransmit the frame because the sender cannot know how many bytes the receiver accepted.

## End-to-End Latency Statistics

Protocol version 3 carries a host-side capture handoff timestamp recorded immediately after `VIDIOC_DQBUF`. The sender keeps the timestamp with the `Frame`, places it in `FrameHeader`, and the receiver measures latency when the complete payload has arrived and passed CRC validation.

Sender JSON reports:

- `capture_to_consumer_*`: capture handoff to consumer dequeue;
- `capture_to_send_*`: capture handoff to completion of header and payload `send_all` calls.

Receiver JSON reports:

- `e2e_latency_*`: capture handoff to complete payload reception and CRC validation;
- `latency_clock_errors`: samples skipped because the receive clock was earlier than the transmitted timestamp.

Each latency group contains sample count, minimum, mean, P50, P95, P99, maximum, and jitter in microseconds. Jitter is defined as the population standard deviation of latency samples.

Same-host tests use the same system clock and can be interpreted directly. Cross-device measurements require synchronized clocks such as NTP or PTP; otherwise receiver-side end-to-end values must not be presented as strict physical latency.

Run the host-only latency regression without a camera:

```bash
./scripts/tcp_latency_regression_test.sh
```

Run the real V4L2 test with latency sample-count and percentile validation:

```bash
./scripts/tcp_v4l2_regression_test.sh
```
