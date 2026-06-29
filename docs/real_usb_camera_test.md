# 真实 USB 摄像头测试记录

## 1. 测试环境

本次测试使用真实 USB 摄像头接入 Ubuntu 虚拟机环境。

* 摄像头类型：UVC USB Web Camera
* 设备节点：`/dev/video0`
* 驱动：`uvcvideo`
* 运行环境：Ubuntu in VMware
* VMware USB 模式：USB 3.0
* 项目程序：`v4l2_capture`

摄像头对应的 V4L2 能力如下：

* 支持 `V4L2_CAP_VIDEO_CAPTURE`
* 支持 `V4L2_CAP_STREAMING`
* 使用 MMAP buffer 进行视频采集

## 2. 摄像头支持格式

通过 `v4l2-ctl` 和项目程序枚举，摄像头支持以下格式：

### MJPG

* 1920x1080 @ 30 fps
* 1280x720 @ 30 fps
* 960x544 @ 30 fps
* 800x480 @ 30 fps
* 640x360 @ 30 fps

### YUYV

* 1920x1080 @ 10 fps
* 1280x720 @ 15 fps
* 960x544 @ 30 fps
* 800x480 @ 30 fps
* 640x360 @ 30 fps

当前项目主要验证 YUYV 采集链路，因此本次真实摄像头测试优先使用 `640x360 YUYV`。

## 3. v4l2-ctl 基础验证

测试命令：

```bash
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=640,height=360,pixelformat=YUYV \
  --set-parm=30 \
  --stream-mmap=4 \
  --stream-count=120 \
  --stream-poll
```

测试结果：

* 帧率稳定在约 30 fps
* 120 帧可以正常采集完成
* 摄像头基础 V4L2 采集链路正常

## 4. 项目程序连续采集测试

测试命令：

```bash
./build/v4l2_capture \
  --device /dev/video0 \
  --width 640 \
  --height 360 \
  --format YUYV \
  --mmap-buffers 4 \
  --frames 300
```

测试结果：

* 请求采集帧数：300
* 实际采集帧数：300
* 实际 FPS：29.059
* 平均帧间隔：33.322 ms
* P50 帧间隔：32.113 ms
* P95 帧间隔：36.157 ms
* P99 帧间隔：36.277 ms
* 最大帧间隔：36.508 ms
* poll timeout：0
* DQBUF error：0
* 最小 bytesused：460800
* 最大 bytesused：460800
* 平均 bytesused：460800

结论：

在 640x360 YUYV 格式下，真实 USB 摄像头可以稳定完成 300 帧连续采集。每帧 `bytesused` 均为 460800 字节，符合 `640 * 360 * 2` 的 YUYV 数据大小，说明采集到的是完整帧。

## 5. Pipeline 保存真实画面测试

测试命令：

```bash
./build/v4l2_capture \
  --device /dev/video0 \
  --width 640 \
  --height 360 \
  --format YUYV \
  --mmap-buffers 4 \
  --pipeline-frames 60 \
  --ring-capacity 8 \
  --pipeline-save \
  --save-limit 5 \
  --output output_usb_cam
```

测试结果：

* producer 产生帧数：60
* consumer 消费帧数：60
* ring buffer 丢帧数：0
* 保存帧数：5
* 成功保存 YUYV 文件：5 个
* 成功保存 PPM 文件：5 个
* 单帧 YUYV 大小：460800 字节
* 单帧 PPM 大小：约 691200 字节

结论：

真实 USB 摄像头采集到的 YUYV 数据可以被 producer-consumer pipeline 正常处理，并成功保存为 `.YUYV` 原始数据和 `.ppm` 可视化图片。

## 6. VMware USB 透传问题排查

最开始使用不稳定的 VMware USB 模式时，真实摄像头出现过以下问题：

* Cheese 打开摄像头时画面花屏
* `v4l2-ctl` 采集速度异常
* 项目程序统计出的 FPS 明显高于摄像头标称帧率
* `bytesused` 出现几百字节、几千字节等异常值
* 保存 PPM 时出现 `YUYV frame data is smaller than expected`

后来将 VMware USB 模式切换为 USB 3.0 后，问题消失：

* `ffplay` 可以正常显示摄像头实时画面
* `v4l2-ctl` 可以稳定采集 30 fps
* 项目程序中 `bytesused` 稳定为 460800
* 保存出的 PPM 图片正常
* `poll timeout` 和 `dqbuf error` 均为 0

结论：

之前的花屏和帧数据异常主要由 VMware USB 透传模式不稳定导致，不是项目 V4L2 采集链路本身的问题。

## 7. 本阶段结论

本阶段完成了真实 USB 摄像头接入验证：

* 识别真实 UVC 摄像头 `/dev/video0`
* 完成 V4L2 capability 查询
* 完成格式设置
* 完成 MMAP buffer 初始化
* 完成 STREAMON / DQBUF / QBUF / STREAMOFF 采集流程
* 完成 300 帧连续采集统计
* 完成 producer-consumer pipeline 测试
* 完成真实画面 YUYV 和 PPM 保存
* 完成 VMware USB 透传异常排查

该测试证明项目不仅可以在 v4l2loopback 虚拟摄像头下运行，也可以接入真实 USB 摄像头完成 V4L2 采集链路验证。

