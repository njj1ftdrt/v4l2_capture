# x86_64 to ARM64 real-network acceptance

This stage proves that the x86_64 development VM and a native ARM64 cloud host
exchange protocol-v3 frames across a real network, not loopback and not an
offline fixture.

## Recommended temporary cloud host

Use an Ubuntu 24.04 ARM64 EC2 instance with instance type `t4g.small`. The
instance has an advertised T4g trial, but public IPv4 addresses and excess CPU
credits may still be billed. Create the instance only for the acceptance window,
check the billing page, and terminate it immediately afterwards. Configure only
two inbound rules, both restricted to the current public IP of the x86 client:

- SSH, TCP 22, source `My IP`.
- Custom TCP, port 19000, source `My IP`.

Do not expose SSH or the test port to `0.0.0.0/0`. The receiver accepts one
session and exits, and `--discard-payload` avoids writing hundreds of camera
frames to the cloud disk.

## Local prerequisites

```bash
sudo apt-get update
sudo apt-get install -y openssh-client git cmake build-essential file python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## Synthetic acceptance first

```bash
SSH_TARGET=ubuntu@ARM_PUBLIC_IP \
SSH_KEY="$HOME/Downloads/v4l2-arm.pem" \
MODE=synthetic \
FRAMES=300 \
PORT=19000 \
./scripts/cross_machine_cloud_acceptance.sh
```

The script uploads the exact Git commit over SSH, builds and runs all tests on
the ARM64 host, starts the ARM receiver, sends deterministic frames from x86,
downloads both evidence sets, and checks sent/received counts and protocol
errors.

## Real-camera acceptance second

```bash
SSH_TARGET=ubuntu@ARM_PUBLIC_IP \
SSH_KEY="$HOME/Downloads/v4l2-arm.pem" \
MODE=camera \
DEVICE=/dev/video0 \
FRAMES=300 \
PORT=19000 \
./scripts/cross_machine_cloud_acceptance.sh
```

For camera mode, the sender-side V4L2 accounting is checked and the ARM receiver
must receive exactly `tcp_sent_frames`. Driver-marked invalid camera buffers are
allowed to be rejected before TCP; CRC, header, rejected-frame, send-error, and
send-timeout counters must remain zero.

## Evidence

The combined manifest is written to:

```text
output/cross-machine-cloud-MODE/evidence/combined_manifest.json
```

The directory and its SHA-256 manifest are packaged as:

```text
output/cross-machine-cloud-MODE.tar.gz
output/cross-machine-cloud-MODE.tar.gz.sha256
```

After the test, remove the Custom TCP rule and terminate the cloud instance to
avoid unnecessary exposure, public-IPv4 charges, storage charges, or CPU-credit
charges.
