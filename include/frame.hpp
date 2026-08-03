#pragma once

#include <linux/videodev2.h>

#include <chrono>
#include <cstdint>
#include <vector>

struct Frame {
    std::vector<std::uint8_t> data;

    __u32 width{0};
    __u32 height{0};
    __u32 pixel_format{0};

    __u32 bytesused{0};
    __u32 sequence{0};
    __u32 buffer_index{0};
    __u32 v4l2_flags{0};
    timeval v4l2_timestamp{};

    // Host-side capture handoff timestamps recorded immediately after DQBUF.
    // system_clock is propagated through the wire protocol for cross-process latency.
    std::uint64_t capture_timestamp_ns{0};
    std::chrono::steady_clock::time_point host_receive_time{};
};
