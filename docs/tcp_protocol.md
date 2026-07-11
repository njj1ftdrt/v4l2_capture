# TCP Frame Transport Protocol

## Goal

This module sends image frames through TCP.

Each frame uses:

FrameHeader + Payload

The receiver must not assume that one recv call can receive a full frame.

TCP is a byte stream protocol. It may produce sticky packets or half packets.

Therefore, receiver must:

1. read fixed-size FrameHeader;
2. parse payload_size;
3. read exactly payload_size bytes;
4. save payload after a complete frame is received.

## Current protocol fields

FrameHeader includes:

- magic
- header_size
- version
- frame_id
- timestamp_ns
- width
- height
- pixel_format
- payload_size

## Current test configuration

- width: 640
- height: 360
- format: YUYV
- payload size: 640 * 360 * 2 = 460800 bytes

## Development steps

1. add frame_protocol.hpp
2. add tcp_receiver.cpp
3. add tcp_sender.cpp with fake test frame
4. connect tcp sender into V4L2 pipeline

## Defensive Header Validation

The receiver validates a complete `FrameHeader` before allocating the payload buffer.

Validation includes:

- protocol magic;
- fixed header size;
- protocol version;
- nonzero and bounded width/height;
- nonzero pixel format;
- nonzero payload size;
- configurable maximum payload size;
- even width for YUYV;
- exact `width * height * 2` payload size for YUYV.

The default payload limit is 16 MiB and can be changed with:

```bash
./build/tcp_receiver \
  --port 9000 \
  --output output/tcp_recv \
  --max-payload-bytes 16777216
```

Invalid headers are rejected before `std::vector` payload allocation. The receiver reports `header_errors`, `rejected_frames`, and `last_error` in `receiver_stats.json`.

## Session Model

The protocol remains a frame stream inside one TCP connection. The receiver may accept multiple TCP connections sequentially when started with `--max-sessions N`.

Statistics and saved-file indices are cumulative across sessions. Test senders may restart their `frame_id` sequence in a new session; the receiver's global file index prevents filename collisions.

A protocol-header or CRC failure remains fatal for the receiver process. Multi-session recovery currently applies to clean connection closure, not to corrupted streams.
