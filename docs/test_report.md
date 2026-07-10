
## Day 2：Pipeline invalid frame 修复验证

### 测试命令

```bash
./build/v4l2_capture \
  --device /dev/video0 \
  --width 640 \
  --height 360 \
  --format YUYV \
  --mmap-buffers 4 \
  --pipeline-frames 300 \
  --ring-capacity 8 \
  --pipeline-save \
  --save-limit 5 \
  --output output/day2_pipeline_fixed \
  --timeout-ms 2000
[200~测试结果
produced frames: 300
consumed frames: 300
ring dropped frames: 0
invalid frames: 1
saved frames: 5
producer FPS: 29.134
consumer FPS: 29.134
output: 成功保存 5 组 YUYV + PPM 图像~
[WARN] skip invalid frame sequence=0 bytesused=3060 data_size=3060 reason=incomplete YUYV frame: got=3060, expected=460800
结论

Pipeline 已能跳过 USB 摄像头首帧不完整数据，并继续完成 300 帧采集、消费和保存流程。

## TCP V4L2 Pipeline Transmission Test

Date: 2026-07-08

### Command

Receiver:

```bash
./build/tcp_receiver \
  --port 9000 \
  --output output/tcp_recv_v4l2 \
  --max-frames 30
./build/v4l2_capture \
  --device /dev/video0 \
  --width 640 \
  --height 360 \
  --format YUYV \
  --mmap-buffers 4 \
  --pipeline-frames 30 \
  --ring-capacity 8 \
  --tcp-host 127.0.0.1 \
  --tcp-port 9000 \
  --timeout-ms 2000
produced frames      : 30
consumed frames      : 30
ring dropped frames  : 0
remaining ring size  : 0
elapsed seconds      : 1.329
producer FPS         : 22.566
consumer FPS         : 22.566
saved frames         : 0
tcp sent frames      : 30
tcp sent bytes       : 13825200
invalid frames       : 0
consumed bytes       : 13824000
640 * 360 * 2 = 460800 bytes
output/tcp_recv_v4l2/recv_frame_10_id_9.YUYV 460800 bytes
output/tcp_recv_v4l2/recv_frame_11_id_10.YUYV 460800 bytes
output/tcp_recv_v4l2/recv_frame_12_id_11.YUYV 460800 bytes
