
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

## PipelineStats 300-Frame Clean Run

### Purpose

This test verifies that the centralized PipelineStats integration does not change the behavior of the existing V4L2 + dual RingBuffer + TCP three-thread pipeline.

### Result

```text
produced frames      : 300
consumed frames      : 300
ring dropped frames  : 0
remaining ring size  : 0
tcp queued frames    : 300
tcp queue dropped    : 0
tcp queue remaining  : 0
tcp sent frames      : 300
tcp sent bytes       : 138252000
tcp send errors      : 0
received frames      : 0
reconnect count      : 0
invalid frames       : 0
consumed bytes       : 138240000
received YUYV files  : 300
each file size       : 460800 bytes

再追加 dev notes：

```bash
cat >> docs/dev_notes.md <<'EOF'

## PipelineStats Validation Note

PipelineStats was validated with a clean 300-frame V4L2 + TCP transmission test.

The final clean run produced:

```text
produced=300
consumed=300
ring_dropped=0
tcp_queued=300
tcp_dropped=0
tcp_sent=300
tcp_send_errors=0
invalid=0
received_files=300
payload_size=460800

然后提交：

```bash
git add include/pipeline_stats.hpp \
  src/main.cpp \
  docs/dev_notes.md \
  docs/test_report.md \
  docs/logs/pipeline_stats_test_ring_buffer.txt \
  docs/logs/pipeline_stats_receiver_300.txt \
  docs/logs/pipeline_stats_sender_300.txt

git commit -m "feat: add centralized pipeline statistics"
ls docs/logs | grep pipeline_stats

## Machine-Readable Statistics Test

### Purpose

This step verifies that the project can export machine-readable JSON statistics for both the V4L2 sender pipeline and the TCP receiver, and that the regression script can validate the end-to-end data path from JSON instead of parsing text logs.

### Verified Items

- Sender writes `pipeline_stats.json`.
- Receiver writes `receiver_stats.json`.
- JSON files are written atomically.
- Regression script reads JSON metrics with Python standard library.
- Logger format changes do not break automated regression.
- Existing V4L2 + dual RingBuffer + TCP + CRC32 behavior remains unchanged.

### Engineering Meaning

This update decouples automation from human-readable logs and makes the regression test more stable and maintainable. It also prepares the project for future monitoring, dashboard integration, and ROS2 status publishing.

## Malformed TCP Header Rejection Test

### Purpose

Verify that untrusted protocol metadata is rejected before payload allocation and file creation.

### Fault Cases

- bad magic;
- unsupported protocol version;
- invalid header size;
- zero width;
- payload exceeding the configured limit;
- YUYV payload-size mismatch.

### Pass Criteria

For every malformed header:

```text
receiver exit code = 3
received_frames = 0
received_bytes = 0
crc_errors = 0
header_errors = 1
rejected_frames = 1
saved_files = 0
```

The receiver must also produce no YUYV file and leave no temporary JSON file.

## Sequential Receiver Session Regression

The receiver was started with a total target of five frames and a maximum of two sessions. The first synthetic sender transmitted two frames and disconnected. The same receiver process then accepted a second sender, received three additional frames, and exited after reaching the cumulative frame target.

Acceptance criteria:

- five valid frames received and saved;
- two sessions accepted and completed;
- one peer disconnect observed before the global frame target;
- no CRC or header errors;
- no rejected frames;
- no unexpected file sizes or temporary JSON files.

## Bounded TCP Connection Retry Test

### Delayed Receiver

The sender starts before the receiver. The receiver is launched after a controlled delay. The sender must recover within the configured maximum attempts and deliver three valid YUYV frames.

Verified conditions:

- connection succeeds after more than one attempt;
- scheduled retry count equals successful attempt minus one;
- three frames are received and saved;
- CRC and header error counts remain zero;
- every saved YUYV file has the expected size.

### Retry Exhaustion

No receiver is started. The sender is configured for three total attempts and must terminate with a non-zero status after exactly three failed attempts.

### Real V4L2 Delayed Receiver

The real camera pipeline starts before the receiver. The producer and consumer threads begin only after connection recovery, preventing TCP queue overflow during receiver startup delay.
