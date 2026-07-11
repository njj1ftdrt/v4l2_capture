# v0.2 CRC Regression Baseline

## Version

Tag:

```text
v0.2-crc-regression
USB Camera
  ↓
Linux V4L2 open/ioctl/mmap/poll/DQBUF/QBUF
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
FrameHeader v2 + Payload + CRC32
  ↓
TCP Receiver
  ↓
CRC32 Verification
  ↓
YUYV File Reconstruction
device              : /dev/video0
resolution          : 640x360
format              : YUYV
payload size        : 460800 bytes
FrameHeader size    : 44 bytes
pipeline frames     : 300
main ring capacity  : 8
tcp queue capacity  : 8
tcp target          : 127.0.0.1:9000
produced frames      : 300
consumed frames      : 300
tcp sent frames      : 300
tcp send errors      : 0
invalid frames       : 0
ring dropped frames  : 0
tcp queue dropped    : 0
receiver frames      : 300
crc errors           : 0
received files       : 300
bad size files       : 0
expected file size   : 460800
docs/logs/v0.2_ring_buffer_test.txt
docs/logs/v0.2_crc_full_regression.txt

---

## 7. 查看当前需要提交的内容

```bash
cd ~/v4l2_capture

git status --short
git diff --stat
git ls-files --others --exclude-standard
cd ~/v4l2_capture

git add \
  CMakeLists.txt \
  README.md \
  include \
  src \
  tests \
  scripts \
  config \
  docs
git status
git diff --cached --stat
git commit -m "test: freeze crc32 regression baseline"
nothing to commit, working tree clean
