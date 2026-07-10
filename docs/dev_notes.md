
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
