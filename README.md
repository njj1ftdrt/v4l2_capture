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
