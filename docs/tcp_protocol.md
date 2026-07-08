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
