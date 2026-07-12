#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace v4l2_ros2_adapter {

inline std::uint8_t clamp_u8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<std::uint8_t>(value);
}

inline std::vector<std::uint8_t> convert_yuyv_to_rgb8(
    const std::vector<std::uint8_t>& payload,
    std::uint32_t width,
    std::uint32_t height
) {
    const std::size_t expected =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 2u;
    if (payload.size() != expected) {
        throw std::invalid_argument(
            "YUYV payload size does not match width and height"
        );
    }
    if ((width % 2u) != 0u) {
        throw std::invalid_argument("YUYV width must be even");
    }

    std::vector<std::uint8_t> rgb(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u
    );

    std::size_t source = 0;
    std::size_t destination = 0;
    while (source < payload.size()) {
        const int y0 = static_cast<int>(payload[source + 0]);
        const int u = static_cast<int>(payload[source + 1]);
        const int y1 = static_cast<int>(payload[source + 2]);
        const int v = static_cast<int>(payload[source + 3]);

        const int d = u - 128;
        const int e = v - 128;

        const auto write_pixel = [&](int y) {
            const int c = y > 16 ? y - 16 : 0;
            const int red = (298 * c + 409 * e + 128) >> 8;
            const int green = (298 * c - 100 * d - 208 * e + 128) >> 8;
            const int blue = (298 * c + 516 * d + 128) >> 8;

            rgb[destination + 0] = clamp_u8(red);
            rgb[destination + 1] = clamp_u8(green);
            rgb[destination + 2] = clamp_u8(blue);
            destination += 3;
        };

        write_pixel(y0);
        write_pixel(y1);
        source += 4;
    }

    return rgb;
}


inline std::vector<std::uint8_t> convert_yuyv_to_mono8(
    const std::vector<std::uint8_t>& payload,
    std::uint32_t width,
    std::uint32_t height
) {
    const std::size_t expected =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) *
        2u;

    if (payload.size() != expected) {
        throw std::invalid_argument(
            "YUYV payload size does not match width and height"
        );
    }

    if ((width % 2u) != 0u) {
        throw std::invalid_argument(
            "YUYV width must be even"
        );
    }

    std::vector<std::uint8_t> mono(
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height)
    );

    std::size_t source = 0;
    std::size_t destination = 0;

    while (source < payload.size()) {
        mono[destination++] = payload[source + 0];
        mono[destination++] = payload[source + 2];
        source += 4;
    }

    return mono;
}

}  // namespace v4l2_ros2_adapter
