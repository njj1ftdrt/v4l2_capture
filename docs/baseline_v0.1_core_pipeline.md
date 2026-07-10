# v0.1 Core Pipeline Baseline

## Version

Tag: v0.1-core-pipeline

## Goal

Freeze the current stable V4L2 + dual RingBuffer + TCP three-thread pipeline as the first stable baseline.

## Architecture

```text
USB Camera
  ↓
V4L2 open/ioctl/mmap/poll/DQBUF/QBUF
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
FrameHeader + Payload
  ↓
TCP Receiver
  ↓
YUYV File Reconstruction
device              : /dev/video0
resolution          : 640x360
format              : YUYV
payload size        : 460800 bytes
pipeline frames     : 300
main ring capacity  : 8
tcp queue capacity  : 8
tcp target          : 127.0.0.1:9000
docs/logs/v0.1_test_ring_buffer.txt
docs/logs/v0.1_tcp_v4l2_regression.txt
docs/logs/v0.1_receiver_300.txt
docs/logs/v0.1_sender_300.txt

---

## 第 8 步：把基线记录写入 dev_notes

```bash
cat >> docs/dev_notes.md <<'EOF'

## v0.1 Core Pipeline Baseline

The current stable baseline is V4L2 + dual RingBuffer + TCP three-thread pipeline.

Frozen features:

- V4L2 camera capture
- main RingBuffer for producer/consumer decoupling
- TCP RingBuffer for asynchronous network sending
- dedicated TCP sender thread
- FrameHeader + Payload protocol
- send_all/read_exact TCP byte stream handling
- invalid YUYV frame filtering
- TCP send error handling
- automated regression test

The next engineering steps must not change the protocol or thread model unless a new baseline is created.
