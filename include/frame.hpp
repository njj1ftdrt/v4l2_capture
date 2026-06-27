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
    timeval v4l2_timestamp{};

    std::chrono::steady_clock::time_point host_receive_time{};
};
