#include "camera_device.hpp"

#include <linux/videodev2.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

static __u32 parse_u32_arg(const std::string& value, const std::string& name) {
    try {
        size_t pos = 0;
        unsigned long parsed = std::stoul(value, &pos, 10);

        if (pos != value.size()) {
            throw std::runtime_error("invalid trailing characters");
        }

        if (parsed == 0 || parsed > 100000) {
            throw std::runtime_error("out of valid range");
        }

        return static_cast<__u32>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid value for " + name + ": " + value);
    }
}

static int parse_int_arg(const std::string& value, const std::string& name) {
    try {
        size_t pos = 0;
        long parsed = std::stol(value, &pos, 10);

        if (pos != value.size()) {
            throw std::runtime_error("invalid trailing characters");
        }

        if (parsed <= 0 || parsed > 60000) {
            throw std::runtime_error("out of valid range");
        }

        return static_cast<int>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid value for " + name + ": " + value);
    }
}

static void print_usage(const char* program) {
    std::cout << "Usage:\n"
              << "  " << program << " --device /dev/video10\n"
              << "  " << program << " --device /dev/video10 --list-formats\n"
              << "  " << program << " --device /dev/video10 --width 640 --height 480 --format YUYV\n"
              << "  " << program << " --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4\n"
              << "  " << program << " --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --capture-one\n"
              << "  " << program << " --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --save-one --output output\n"
              << "  " << program << " --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --frames 300 --timeout-ms 2000\n"
              << "  " << program << " --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --capture-frames 300 --timeout-ms 2000\n";
}

int main(int argc, char* argv[]) {
    std::string device = "/dev/video10";
    bool list_formats = false;
    bool capture_one = false;
    bool save_one = false;
    int capture_frames = 0;
    int timeout_ms = 2000;
    std::string output_dir = "output";

    std::optional<__u32> width;
    std::optional<__u32> height;
    std::optional<std::string> pixel_format;
    std::optional<__u32> mmap_buffers;

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];

            if ((arg == "--device" || arg == "-d") && i + 1 < argc) {
                device = argv[++i];
            } else if (arg == "--list-formats") {
                list_formats = true;
            } else if (arg == "--width" && i + 1 < argc) {
                width = parse_u32_arg(argv[++i], "--width");
            } else if (arg == "--height" && i + 1 < argc) {
                height = parse_u32_arg(argv[++i], "--height");
            } else if (arg == "--format" && i + 1 < argc) {
                pixel_format = argv[++i];
            } else if (arg == "--mmap-buffers" && i + 1 < argc) {
                mmap_buffers = parse_u32_arg(argv[++i], "--mmap-buffers");
            } else if (arg == "--capture-one") {
                capture_one = true;
            } else if (arg == "--save-one") {
                save_one = true;
            } else if ((arg == "--frames" || arg == "--capture-frames") && i + 1 < argc) {
                capture_frames = parse_int_arg(argv[++i], arg);
            } else if (arg == "--output" && i + 1 < argc) {
                output_dir = argv[++i];
            } else if (arg == "--timeout-ms" && i + 1 < argc) {
                timeout_ms = parse_int_arg(argv[++i], "--timeout-ms");
            } else if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                return 0;
            } else {
                std::cerr << "Unknown or incomplete argument: " << arg << "\n";
                print_usage(argv[0]);
                return 1;
            }
        }

        const bool wants_set_format =
            width.has_value() || height.has_value() || pixel_format.has_value();

        if (wants_set_format &&
            !(width.has_value() && height.has_value() && pixel_format.has_value())) {
            throw std::runtime_error(
                "Setting format requires --width, --height and --format together"
            );
        }

        if (mmap_buffers.has_value() && !wants_set_format) {
            throw std::runtime_error(
                "Initializing MMAP buffers requires setting format first"
            );
        }

        const int capture_mode_count =
            (capture_one ? 1 : 0) +
            (save_one ? 1 : 0) +
            (capture_frames > 0 ? 1 : 0);

        if (capture_mode_count > 1) {
            throw std::runtime_error(
                "Use only one capture mode: --capture-one, --save-one, or --frames"
            );
        }

        if (capture_mode_count > 0 && !mmap_buffers.has_value()) {
            throw std::runtime_error(
                "Capturing frames requires --mmap-buffers"
            );
        }

        CameraDevice camera(device);
        camera.open_device();
        camera.query_capability();

        if (list_formats) {
            camera.list_formats();
        }

        if (wants_set_format) {
            camera.set_format(*width, *height, *pixel_format);
        }

        if (mmap_buffers.has_value()) {
            camera.init_mmap_buffers(*mmap_buffers);
        }

        if (capture_mode_count > 0) {
            camera.start_streaming();

            if (save_one) {
                camera.capture_one_frame_to_files(timeout_ms, output_dir);
            } else if (capture_one) {
                camera.capture_one_frame(timeout_ms);
            } else {
                camera.capture_frames(capture_frames, timeout_ms);
            }

            camera.stop_streaming();
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }

    return 0;
}
