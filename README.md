# V4L2 摄像头采集与性能分析系统

本项目是一个 Linux 用户态 V4L2 摄像头采集项目，目标是直接使用 V4L2 接口完成摄像头采集，而不是使用 OpenCV VideoCapture。

当前测试使用虚拟摄像头 /dev/video10，真实 USB 摄像头测试会在后续补充。

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

- open 打开 V4L2 设备
- VIDIOC_QUERYCAP 查询设备能力
- VIDIOC_ENUM_FMT 查询支持格式
- VIDIOC_ENUM_FRAMESIZES 查询分辨率
- VIDIOC_ENUM_FRAMEINTERVALS 查询帧率
- VIDIOC_S_FMT 设置采集格式
- VIDIOC_G_FMT 验证实际格式
- VIDIOC_REQBUFS 申请 MMAP 缓冲区
- mmap 映射驱动缓冲区
- VIDIOC_QBUF 入队缓冲区
- VIDIOC_STREAMON 开启采集
- poll 等待帧到达
- VIDIOC_DQBUF 取出帧
- VIDIOC_STREAMOFF 停止采集
- 保存 YUYV 原始帧
- 将 YUYV 转换为 PPM 图像
- 连续采集 300 帧并统计 FPS、P50、P95、P99、最大帧间隔
- 实现生产者消费者 Pipeline
- 实现固定容量 RingBuffer
- 支持慢 consumer 丢帧测试
- 支持 Pipeline consumer 保存少量帧
- 支持设备不存在、格式不支持、输出目录不可写等异常测试

## 编译

cmake -S . -B build
cmake --build build -j

## RingBuffer 测试

g++ -std=c++17 -Wall -Wextra -Iinclude tests/test_ring_buffer.cpp -o build/test_ring_buffer
./build/test_ring_buffer

预期输出：RingBuffer tests passed.

## 查询格式

命令：./build/v4l2_capture --device /dev/video10 --list-formats

实测结果：
[0] YUYV - YUYV 4:2:2
  size[0]: 640x480
    interval[0]: 1/30 s (30 fps)

## 保存单帧

命令：./build/v4l2_capture --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --save-one --output output --timeout-ms 2000

输出文件：
- output/frame_000000.YUYV
- output/frame_000000.ppm

## 连续采集 300 帧

命令：./build/v4l2_capture --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --frames 300 --timeout-ms 2000

一次实测结果：
- captured frames：300
- actual FPS：30.120
- avg interval ms：33.312
- p50 interval ms：33.363
- p95 interval ms：34.521
- p99 interval ms：35.122
- max interval ms：35.702
- poll timeouts：0
- dqbuf errors：0
- bytesused：614400

说明：这里统计的是主机程序收到帧之间的 steady_clock 时间间隔，不是摄像头硬件曝光时间，也不是端到端延迟。

## 多线程 Pipeline

结构：采集线程 DQBUF 后复制 MMAP buffer 到 Frame::data，然后立即 QBUF 还给驱动，再 push 到 RingBuffer；consumer 从 RingBuffer 取出 Frame，可选保存少量帧。

关键设计点：consumer 不直接读取 MMAP buffer 指针，而是处理 producer 拷贝出来的 Frame 数据副本。因为 MMAP buffer 一旦 QBUF 还给驱动，驱动后续可能重新写入那块内存。

## 正常 Pipeline 测试

命令：./build/v4l2_capture --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --pipeline-frames 300 --ring-capacity 8 --timeout-ms 2000

实测结果：
- produced frames：300
- consumed frames：300
- ring dropped frames：0
- producer FPS：30.179
- consumer FPS：30.179
- consumed bytes：184320000

## 慢 consumer 丢帧测试

命令：./build/v4l2_capture --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --pipeline-frames 300 --ring-capacity 2 --consumer-delay-ms 50 --timeout-ms 2000

实测结果：
- produced frames：300
- consumed frames：199
- ring dropped frames：101
- producer FPS：29.872
- consumer FPS：19.815

这个测试证明：当 consumer 处理速度慢于 producer 时，固定容量 RingBuffer 会覆盖旧帧，并记录 dropped_count。

## Pipeline 保存测试

命令：./build/v4l2_capture --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --pipeline-frames 300 --ring-capacity 8 --pipeline-save --save-limit 5 --output output/pipeline --timeout-ms 2000

实测结果：
- produced frames：300
- consumed frames：300
- ring dropped frames：0
- saved frames：5
- consumed bytes：184320000

输出文件：5 个 .YUYV 文件和 5 个 .ppm 文件。

## 异常测试

设备不存在：./build/v4l2_capture --device /dev/video999 --width 640 --height 480 --format YUYV --mmap-buffers 4 --frames 10 --timeout-ms 1000
结果：[ERROR] Failed to open device /dev/video999: No such file or directory

不支持的像素格式：./build/v4l2_capture --device /dev/video10 --width 640 --height 480 --format MJPG --mmap-buffers 4 --frames 10 --timeout-ms 1000
结果：[ERROR] VIDIOC_S_FMT failed: Invalid argument

输出目录不可写：./build/v4l2_capture --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --pipeline-frames 10 --ring-capacity 4 --pipeline-save --save-limit 1 --output /root/v4l2_pipeline_test --timeout-ms 1000
结果：[ERROR] filesystem error: cannot create directories: Permission denied [/root/v4l2_pipeline_test]

修复后，consumer 保存失败时会通知 producer 尽快停止，避免继续无意义采集。
### 真实 USB 摄像头测试

项目已接入真实 UVC USB 摄像头 `/dev/video0` 进行验证。

在 VMware USB 3.0 透传环境下，使用 `640x360 YUYV` 格式、4 个 MMAP buffer 连续采集 300 帧，测试结果如下：

* 实际 FPS：29.059
* 平均帧间隔：33.322 ms
* P95 帧间隔：36.157 ms
* P99 帧间隔：36.277 ms
* poll timeout：0
* DQBUF error：0
* bytesused 稳定为 460800 字节

同时验证了 producer-consumer pipeline 在真实摄像头下的保存功能，成功保存 5 张 `.YUYV` 原始图像和 5 张 `.ppm` 可视化图片。

测试过程中曾遇到 VMware USB 透传导致的花屏和帧数据异常问题，切换到 USB 3.0 后恢复正常。详细过程见 `docs/real_usb_camera_test.md`。

