
# TCP Pipeline Engineering Notes

## Current Architecture

The current TCP pipeline uses a three-thread design:

```text
V4L2 producer thread
  → main RingBuffer
  → consumer thread
  → tcp RingBuffer
  → dedicated TCP sender thread

---

# 第五步：补简历描述文件

新建：

```bash id="22249n"
cat > docs/resume_project.md <<'EOF'
# Resume Project Description

## Project Name

Embedded Linux V4L2 Camera Capture and TCP Frame Transport System

## Chinese Version

基于 Linux V4L2 的嵌入式图像采集与 TCP 传输系统

## Resume Bullets

- 基于 Linux V4L2 原生接口实现 USB 摄像头图像采集，完成 open/ioctl/mmap/poll/DQBUF/QBUF 采集流程，支持 YUYV 640×360 原始帧获取与保存。
- 设计 Frame 数据结构与 RingBuffer 生产者-消费者模型，使用多线程解耦摄像头采集、帧处理与网络发送流程，并通过 produced/consumed/dropped/invalid 等指标定位性能瓶颈。
- 设计 FrameHeader + Payload 自定义 TCP 图像帧协议，实现 send_all/read_exact 处理 TCP 粘包、半包问题，接收端可按 payload_size 重建完整 YUYV 文件。
- 将 TCP 发送拆分为独立线程并引入第二级 RingBuffer，补充异常帧过滤、TCP 断线错误处理和自动化回归测试，完成 300 帧真实摄像头传输验证，接收端 300 个 YUYV 文件均为 460800 bytes。

## Short Interview Introduction

这个项目是一个面向嵌入式 Linux 和机器人视觉前端的数据采集与传输系统。我没有使用 OpenCV，而是直接基于 Linux V4L2 接口实现 USB 摄像头采集流程，包括 mmap、poll、DQBUF/QBUF 等底层机制。系统使用 RingBuffer 解耦采集线程和消费线程，并进一步把 TCP 发送拆成独立线程，通过第二级队列减少网络 IO 对主流程的阻塞。网络传输部分我设计了 FrameHeader + Payload 协议，使用 send_all 和 read_exact 处理 TCP 粘包和半包问题，并通过 300 帧真实摄像头测试验证接收端能完整重建 460800 bytes 的 YUYV 图像帧。

## One-Sentence Version

基于 Linux V4L2 实现 USB 摄像头 YUYV 原始帧采集，设计多线程 RingBuffer Pipeline 与 FrameHeader + Payload TCP 协议，完成真实摄像头图像帧的稳定传输、断线处理和自动化回归测试。

## Lightweight Thread-Safe Logger

The project now includes a lightweight thread-safe Logger.

Supported levels:

```text
DEBUG < INFO < WARN < ERROR
[timestamp][level][module] message
[2026-07-10 19:30:21.123][INFO][PIPELINE] tcp sent frames      : 300
config/v4l2_tcp_pipeline.conf
./build/v4l2_capture --config config/v4l2_tcp_pipeline.conf --log-level DEBUG

追加到 `docs/test_report.md`：

```bash
cat >> docs/test_report.md <<'EOF'

## Logger Integration Test

### Purpose

This test verifies that the project can use a lightweight thread-safe Logger without changing the V4L2 + dual RingBuffer + TCP three-thread pipeline behavior.

### Verified Items

- Build passed.
- RingBuffer unit test passed.
- Logger prints timestamp, level, module and message.
- `log_level=INFO` prints normal pipeline information.
- `log_level=DEBUG` prints producer and consumer debug logs.
- TCP/V4L2 300-frame transmission still passes.
- Receiver reconstructs complete YUYV payload files.

### Expected Payload Size

```text
640 * 360 * 2 = 460800 bytes

---

## 第 10 步：提交

```bash
git add CMakeLists.txt \
  include/logger.hpp \
  src/logger.cpp \
  src/app_config.cpp \
  src/main.cpp \
  docs/dev_notes.md \
  docs/test_report.md \
  docs/logs/logger_test_ring_buffer.txt \
  docs/logs/logger_receiver_300.txt \
  docs/logs/logger_sender_300.txt \
  docs/logs/logger_debug_sender_30.txt

git commit -m "feat: add lightweight thread safe logger"

## TCP Payload CRC32 Check

The TCP frame protocol was upgraded from version 1 to version 2.

FrameHeader now includes:

```text
payload_crc32
v1 FrameHeader size: 40 bytes
v2 FrameHeader size: 44 bytes

```bash
cat >> docs/test_report.md <<'EOF'

## TCP Payload CRC32 Test

### Purpose

This test verifies that the TCP receiver can detect payload corruption by comparing the CRC32 value in FrameHeader with the CRC32 value recalculated from the received payload.

### Verified Items

- Build passed.
- RingBuffer unit test passed.
- tcp_sender test frames passed CRC verification.
- V4L2 + TCP pipeline still works after protocol v2 upgrade.
- Receiver reconstructs complete YUYV files.
- Receiver reports `crc errors = 0`.

### Protocol Update

```text
FrameHeader v1: 40 bytes
FrameHeader v2: 44 bytes
New field: payload_crc32
640 * 360 * 2 = 460800 bytes
460800 + 44 = 460844 bytes

---

## 第 8 步：提交

```bash
git add include/frame_protocol.hpp \
  src/main.cpp \
  src/tcp_sender.cpp \
  src/tcp_receiver.cpp \
  docs/dev_notes.md \
  docs/test_report.md \
  docs/logs/crc_test_ring_buffer.txt \
  docs/logs/crc_tcp_receiver_test.txt \
  docs/logs/crc_tcp_sender_test.txt \
  docs/logs/crc_pipeline_receiver_300.txt \
  docs/logs/crc_pipeline_sender_300.txt

git commit -m "feat: add tcp payload crc32 verification"

## Machine-Readable JSON Statistics

The project now separates human-readable logs from machine-readable runtime statistics.

Sender-side pipeline statistics are exported to a JSON file, for example:

```text
output/stats/pipeline_stats.json
```

Receiver-side statistics are exported to:

```text
output/stats/receiver_stats.json
```

Both files are written by first creating a temporary `.tmp` file and then atomically renaming it to the final path. This avoids partially written JSON files being read by automation.

The automated regression script now reads JSON statistics with Python's standard `json` module instead of parsing text logs. This removes coupling between the regression test and Logger output format.
