# AArch64 build and deployment

This project supports two ARM64 evidence paths:

1. Cross-compile on an x86_64 Debian/Ubuntu development host.
2. Native build and acceptance on an AArch64 Linux host or GitHub ARM64 runner.

## Cross-compile on x86_64

Install dependencies:

```bash
sudo apt-get update
sudo apt-get install -y g++-aarch64-linux-gnu cmake ninja-build file
```

Build and package:

```bash
./scripts/aarch64_cross_build.sh
```

Outputs:

- `output/v4l2_capture-aarch64-release.tar.gz`
- `output/v4l2_capture-aarch64-release.tar.gz.sha256`
- `output/aarch64-cross/evidence/file.txt`
- `output/aarch64-cross/evidence/manifest.json`

The script fails unless every expected ELF is identified as `ARM aarch64`.
Cross-compilation proves target-code generation, but does not prove that the
binaries run on ARM hardware.

## Native ARM64 acceptance

On an Ubuntu/Debian ARM64 host:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build file python3
./scripts/aarch64_native_acceptance.sh
```

The script verifies:

- the host reports `aarch64` or `arm64`;
- all CTest tests pass natively;
- `tcp_sender` and `tcp_receiver` complete an ARM64 loopback transfer;
- receiver counts match and CRC/header/rejection counters remain zero;
- platform metadata and binary hashes are stored as evidence.

A cloud ARM VM normally has no UVC camera. It can validate native ARM64 build,
TCP, protocol, tests, resource use, and stability, but not physical V4L2 camera
bring-up. A remote physical board with `/dev/video*` is required for that claim.

## Cross-machine TCP verification

Run the receiver on one machine:

```bash
./build-aarch64-native/tcp_receiver \
  --port 9000 \
  --max-frames 300 \
  --output output/cross_arch_recv \
  --stats-output output/cross_arch_receiver.json
```

Run the sender on the other machine, replacing the address:

```bash
./build/tcp_sender \
  --host ARM_HOST_IP \
  --port 9000 \
  --frames 300 \
  --width 640 \
  --height 360 \
  --interval-ms 10
```

Protocol version 3 now serializes every header field explicitly into a stable
44-byte little-endian wire representation. The CI fixture chain verifies x86_64
to AArch64 parsing, AArch64 to x86_64 parsing, CRC32, and byte-identical output.
See `docs/cross_arch_protocol.md` for the evidence workflow and the separate
real cross-machine network acceptance procedure.

## Automated real-network cloud acceptance

For an end-to-end x86_64-to-ARM64 test over a public network, use
`scripts/cross_machine_cloud_acceptance.sh`. It deploys the exact source commit,
performs native ARM64 build/CTest, runs a one-session receiver, retrieves the
remote statistics, and produces a combined evidence archive. See
`docs/cross_machine_cloud_acceptance.md`.
