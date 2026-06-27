# V4L2 摄像头采集与性能分析系统

本项目是一个 Linux 用户态 V4L2 摄像头采集项目，核心目标是直接使用 Linux V4L2 接口完成摄像头采集，而不是使用 OpenCV VideoCapture。

当前测试使用虚拟摄像头 `/dev/video10`，真实 USB 摄像头测试会在后续补充。

## 当前测试环境

- 系统：Ubuntu 24.04 VMware 虚拟机
- 当前设备：/dev/video10
- 驱动：v4l2loopback
- 设备名称：v4l2-test-cam
- 像素格式：YUYV
- 分辨率：640x480
- 单帧大小：614400 bytes
- 标称帧率：30 FPS

注意：当前结果来自 v4l2loopback 虚拟摄像头，不能写成真实 USB 摄像头结果。

## 已实现功能

- 使用 open() 打开 V4L2 设备
- 使用 VIDIOC_QUERYCAP 查询设备能力
- 使用 VIDIOC_ENUM_FMT 查询支持格式
- 使用 VIDIOC_ENUM_FRAMESIZES 查询分辨率
- 使用 VIDIOC_ENUM_FRAMEINTERVALS 查询帧率
- 使用 VIDIOC_S_FMT 设置采集格式
- 使用 VIDIOC_G_FMT 验证实际格式
- 使用 VIDIOC_REQBUFS 申请 MMAP 缓冲区
- 使用 mmap() 映射驱动缓冲区
- 使用 VIDIOC_QBUF 入队缓冲区
- 使用 VIDIOC_STREAMON 开启采集
- 使用 poll() 等待帧到达
- 使用 VIDIOC_DQBUF 取出帧
- 使用 VIDIOC_QBUF 重新入队
- 使用 VIDIOC_STREAMOFF 停止采集
- 保存 YUYV 原始帧
- 将 YUYV 转换为 PPM 图像
- 连续采集 300 帧并统计 FPS、P50、P95、P99、最大帧间隔
- 实现生产者消费者 Pipeline
- 实现固定容量 RingBuffer
- 支持慢 consumer 丢帧测试
- 支持 Pipeline consumer 保存少量帧
- 支持设备不存在、格式不支持、输出目录不可写等异常测试

## 编译

```bash
cmake -S . -B build
cmake --build build -j
./build/v4l2_capture --device /dev/video10 --list-formats
[0] YUYV - YUYV 4:2:2
  size[0]: 640x480
    interval[0]: 1/30 s (30 fps)
./build/v4l2_capture \
  --device /dev/video10 \
  --width 640 \
  --height 480 \
  --format YUYV \
  --mmap-buffers 4 \
  --save-one \
  --output output \
  --timeout-ms 2000
./build/v4l2_capture \
  --device /dev/video10 \
  --width 640 \
  --height 480 \
  --format YUYV \
  --mmap-buffers 4 \
  --frames 300 \
  --timeout-ms 2000
采集线程 producer:
  DQBUF
  拷贝 MMAP buffer 到 Frame::data
  立即 QBUF 还给驱动
  push Frame 到 RingBuffer

处理线程 consumer:
  从 RingBuffer 取出 Frame
  可选保存少量帧
./build/v4l2_capture \
  --device /dev/video10 \
  --width 640 \
  --height 480 \
  --format YUYV \
  --mmap-buffers 4 \
  --pipeline-frames 300 \
  --ring-capacity 8 \
  --timeout-ms 2000
./build/v4l2_capture \
  --device /dev/video10 \
  --width 640 \
  --height 480 \
  --format YUYV \
  --mmap-buffers 4 \
  --pipeline-frames 300 \
  --ring-capacity 2 \
  --consumer-delay-ms 50 \
  --timeout-ms 2000
rm -rf output/pipeline

./build/v4l2_capture \
  --device /dev/video10 \
  --width 640 \
  --height 480 \
  --format YUYV \
  --mmap-buffers 4 \
  --pipeline-frames 300 \
  --ring-capacity 8 \
  --pipeline-save \
  --save-limit 5 \
  --output output/pipeline \
  --timeout-ms 2000
./build/v4l2_capture \
  --device /dev/video999 \
  --width 640 \
  --height 480 \
  --format YUYV \
  --mmap-buffers 4 \
  --frames 10 \
  --timeout-ms 1000
[ERROR] Failed to open device /dev/video999: No such file or directory
./build/v4l2_capture \
  --device /dev/video10 \
  --width 640 \
  --height 480 \
  --format MJPG \
  --mmap-buffers 4 \
  --frames 10 \
  --timeout-ms 1000
[ERROR] VIDIOC_S_FMT failed: Invalid argument
./build/v4l2_capture \
  --device /dev/video10 \
  --width 640 \
  --height 480 \
  --format YUYV \
  --mmap-buffers 4 \
  --pipeline-frames 10 \
  --ring-capacity 4 \
  --pipeline-save \
  --save-limit 1 \
  --output /root/v4l2_pipeline_test \
  --timeout-ms 1000
[ERROR] filesystem error: cannot create directories: Permission denied [/root/v4l2_pipeline_test]

---

## 检查 README 是否正常

```bash
head -n 30 README.md
tail -n 30 README.md
git status --short
