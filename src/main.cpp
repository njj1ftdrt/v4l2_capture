#include "camera_device.hpp"
#include "frame.hpp"
#include "ring_buffer.hpp"

#include <linux/videodev2.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

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


static void run_pipeline(
    CameraDevice& camera,
    int frame_count,
    int timeout_ms,
    int ring_capacity
) {
    if (frame_count <= 0) {
        throw std::runtime_error("Pipeline frame count must be positive");
    }

    if (ring_capacity <= 0) {
        throw std::runtime_error("Ring capacity must be positive");
    }

    RingBuffer<Frame> ring(static_cast<std::size_t>(ring_capacity));

    std::atomic<bool> producer_done{false};
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<std::uint64_t> consumed_bytes{0};

    std::mutex cv_mutex;
    std::condition_variable cv;

    std::exception_ptr producer_error = nullptr;
    std::exception_ptr consumer_error = nullptr;

    std::cout << "========== Pipeline Capture ==========\n";
    std::cout << "target frames : " << frame_count << "\n";
    std::cout << "ring capacity : " << ring_capacity << "\n";
    std::cout << "poll timeout  : " << timeout_ms << " ms\n";

    const auto start_time = std::chrono::steady_clock::now();

    std::thread consumer_thread([&]() {
        try {
            while (!producer_done.load() || !ring.empty()) {
                Frame frame;

                if (ring.try_pop_oldest(frame)) {
                    ++consumed;
                    consumed_bytes += frame.bytesused;

                    const int count = consumed.load();
                    if (count == 1 || count == frame_count || count % 50 == 0) {
                        std::cout << "[CONSUMER] consumed "
                                  << count
                                  << " sequence=" << frame.sequence
                                  << " bytesused=" << frame.bytesused
                                  << "\n";
                    }

                    continue;
                }

                std::unique_lock<std::mutex> lock(cv_mutex);
                cv.wait_for(lock, std::chrono::milliseconds(100));
            }
        } catch (...) {
            consumer_error = std::current_exception();
        }
    });

    std::thread producer_thread([&]() {
        try {
            for (int i = 0; i < frame_count; ++i) {
                Frame frame = camera.capture_frame_copy(timeout_ms);
                ring.push(std::move(frame));

                const int count = ++produced;
                if (count == 1 || count == frame_count || count % 50 == 0) {
                    std::cout << "[PRODUCER] produced "
                              << count << "/" << frame_count
                              << " ring_size=" << ring.size()
                              << " dropped=" << ring.dropped_count()
                              << "\n";
                }

                cv.notify_one();
            }
        } catch (...) {
            producer_error = std::current_exception();
        }

        producer_done = true;
        cv.notify_all();
    });

    producer_thread.join();
    consumer_thread.join();

    if (producer_error) {
        std::rethrow_exception(producer_error);
    }

    if (consumer_error) {
        std::rethrow_exception(consumer_error);
    }

    const auto end_time = std::chrono::steady_clock::now();
    const double elapsed_s =
        std::chrono::duration<double>(end_time - start_time).count();

    const double producer_fps = elapsed_s > 0.0
        ? static_cast<double>(produced.load()) / elapsed_s
        : 0.0;

    const double consumer_fps = elapsed_s > 0.0
        ? static_cast<double>(consumed.load()) / elapsed_s
        : 0.0;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "========== Pipeline Statistics ==========\n";
    std::cout << "produced frames      : " << produced.load() << "\n";
    std::cout << "consumed frames      : " << consumed.load() << "\n";
    std::cout << "ring dropped frames  : " << ring.dropped_count() << "\n";
    std::cout << "remaining ring size  : " << ring.size() << "\n";
    std::cout << "elapsed seconds      : " << elapsed_s << "\n";
    std::cout << "producer FPS         : " << producer_fps << "\n";
    std::cout << "consumer FPS         : " << consumer_fps << "\n";
    std::cout << "consumed bytes       : " << consumed_bytes.load() << "\n";
    std::cout << "=========================================\n";
    std::cout.unsetf(std::ios::floatfield);
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
              << "  " << program << " --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --capture-frames 300 --timeout-ms 2000\n"
              << "  " << program << " --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --pipeline-frames 300 --ring-capacity 8 --timeout-ms 2000\n";
}

int main(int argc, char* argv[]) {
    std::string device = "/dev/video10";
    bool list_formats = false;
    bool capture_one = false;
    bool save_one = false;
    int capture_frames = 0;
    int pipeline_frames = 0;
    int ring_capacity = 8;
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
            } else if (arg == "--pipeline-frames" && i + 1 < argc) {
                pipeline_frames = parse_int_arg(argv[++i], arg);
            } else if (arg == "--ring-capacity" && i + 1 < argc) {
                ring_capacity = parse_int_arg(argv[++i], arg);
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
            (capture_frames > 0 ? 1 : 0) +
            (pipeline_frames > 0 ? 1 : 0);

        if (capture_mode_count > 1) {
            throw std::runtime_error(
                "Use only one capture mode: --capture-one, --save-one, --frames, or --pipeline-frames"
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
            } else if (capture_frames > 0) {
                camera.capture_frames(capture_frames, timeout_ms);
            } else {
                run_pipeline(camera, pipeline_frames, timeout_ms, ring_capacity);
            }

            camera.stop_streaming();
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }

    return 0;
}
