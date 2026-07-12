#include "v4l2_ros2_adapter/yuyv_conversion.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        const std::vector<std::uint8_t> black_yuyv{16, 128, 16, 128};
        const auto black_rgb = v4l2_ros2_adapter::convert_yuyv_to_rgb8(
            black_yuyv,
            2,
            1
        );
        require(black_rgb.size() == 6, "black conversion size mismatch");
        for (std::uint8_t value : black_rgb) {
            require(value == 0, "neutral black should convert to RGB zero");
        }

        const std::vector<std::uint8_t> white_yuyv{235, 128, 235, 128};
        const auto white_rgb = v4l2_ros2_adapter::convert_yuyv_to_rgb8(
            white_yuyv,
            2,
            1
        );
        for (std::uint8_t value : white_rgb) {
            require(value >= 254, "neutral white should convert near RGB 255");
        }

        bool rejected_bad_size = false;
        try {
            (void)v4l2_ros2_adapter::convert_yuyv_to_rgb8(black_yuyv, 4, 1);
        } catch (const std::invalid_argument&) {
            rejected_bad_size = true;
        }
        require(rejected_bad_size, "bad payload size was not rejected");

        bool rejected_odd_width = false;
        try {
            const std::vector<std::uint8_t> odd_width_payload(6, 128);
            (void)v4l2_ros2_adapter::convert_yuyv_to_rgb8(
                odd_width_payload,
                3,
                1
            );
        } catch (const std::invalid_argument&) {
            rejected_odd_width = true;
        }
        require(rejected_odd_width, "odd YUYV width was not rejected");


        const std::vector<std::uint8_t> mono_source{
            10, 90, 20, 180,
            30, 70, 40, 200
        };

        const auto mono =
            v4l2_ros2_adapter::convert_yuyv_to_mono8(
                mono_source,
                4,
                1
            );

        const std::vector<std::uint8_t> expected_mono{
            10, 20, 30, 40
        };

        require(
            mono == expected_mono,
            "mono conversion values mismatch"
        );

        bool mono_rejected_bad_size = false;

        try {
            (void)v4l2_ros2_adapter::convert_yuyv_to_mono8(
                black_yuyv,
                4,
                1
            );
        } catch (const std::invalid_argument&) {
            mono_rejected_bad_size = true;
        }

        require(
            mono_rejected_bad_size,
            "mono conversion accepted a bad payload size"
        );

        std::cout << "[PASS] YUYV conversion tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
