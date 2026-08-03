# Cross-architecture protocol acceptance

## Goal

Prove that protocol version 3 has one stable 44-byte wire representation and
that x86_64 and AArch64 can generate and consume exactly the same bytes.

This is stronger than checking that both platforms happen to report
`sizeof(FrameHeader) == 44`. The host-side `FrameHeader` is now encoded and
decoded field-by-field in explicit little-endian order. Struct padding and host
endianness are not copied onto the network.

## Wire header layout

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic |
| 4 | 2 | header_size |
| 6 | 2 | version |
| 8 | 8 | frame_id |
| 16 | 8 | capture_timestamp_ns |
| 24 | 4 | width |
| 28 | 4 | height |
| 32 | 4 | pixel_format |
| 36 | 4 | payload_size |
| 40 | 4 | payload_crc32 |

All integer fields use little-endian byte order. The wire size remains 44 bytes,
so existing little-endian protocol-v3 peers remain compatible.

## Golden-byte unit test

`test_frame_protocol` checks a fixed 44-byte sequence for known integer values.
A field offset, byte order, header size, or version change fails the test.

## Deterministic fixture

`protocol_fixture` writes or verifies a deterministic stream containing 32 YUYV
frames by default. Each record contains:

1. Explicitly serialized 44-byte header.
2. Deterministic 8x4 YUYV payload.
3. CRC32 stored in the header and checked during verification.

Local use:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"

./build/protocol_fixture \
  --write output/protocol_fixture.bin \
  --frames 32

./build/protocol_fixture \
  --verify output/protocol_fixture.bin \
  --frames 32
```

Or run the evidence wrapper:

```bash
LABEL=x86_64 ./scripts/protocol_fixture_acceptance.sh
```

## CI evidence chain

The ARM64 workflow performs three independent checks:

1. x86_64 generates and self-verifies a deterministic fixture.
2. Native AArch64 downloads and verifies the x86_64 fixture, then generates its
   own fixture and confirms both files are byte-identical.
3. A fresh x86_64 job downloads the ARM64 fixture, verifies it, and compares it
   with the original x86_64 artifact.

Expected artifacts:

- `x86-protocol-fixture`
- `aarch64-native-acceptance`
- `cross-arch-protocol-evidence`

The final manifest must report:

```text
cross_fixture_verified=true
byte_identical_to_cross_fixture=true
```

The downloaded x86_64 and ARM64 fixture SHA-256 values must also be identical.

## Real cross-machine TCP test

Fixture verification proves binary protocol compatibility without requiring two
machines to be online simultaneously. A real network test is still separate.

On the receiver machine:

```bash
BUILD_DIR=build-aarch64-native \
PORT=9000 \
FRAMES=300 \
./scripts/cross_machine_tcp_acceptance.sh receiver
```

On the sender machine:

```bash
BUILD_DIR=build \
TARGET_HOST=ARM_HOST_IP \
PORT=9000 \
FRAMES=300 \
./scripts/cross_machine_tcp_acceptance.sh sender
```

Then reverse the roles to validate both directions. A passing receiver requires
all expected frames and zero CRC, header, and rejected-frame errors.

Cross-machine latency must not be reported unless both hosts have verified clock
synchronization. The receiver timestamp calculation uses the two machines'
system clocks.
