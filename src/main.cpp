#include "camera_device.hpp"
#include "frame.hpp"
#include "ring_buffer.hpp"

#include <linux/videodev2.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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



static std::string frame_fourcc_to_string(__u32 pixelformat) {
    std::string s;
    s.push_back(static_cast<char>(pixelformat & 0xFF));
    s.push_back(static_cast<char>((pixelformat >> 8) & 0xFF));
    s.push_back(static_cast<char>((pixelformat >> 16) & 0xFF));
    s.push_back(static_cast<char>((pixelformat >> 24) & 0xFF));
    return s;
}

static unsigned char frame_clamp_to_u8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<unsigned char>(value);
}

static void frame_yuyv_to_rgb(
    const std::uint8_t* yuyv,
    std::vector<std::uint8_t>& rgb,
    __u32 width,
    __u32 height
) {
    rgb.resize(static_cast<std::size_t>(width) * height * 3);

    std::size_t in = 0;
    std::size_t out = 0;
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;

    for (std::size_t i = 0; i + 1 < pixel_count; i += 2) {
        int y0 = yuyv[in + 0];
        int u  = yuyv[in + 1];
        int y1 = yuyv[in + 2];
        int v  = yuyv[in + 3];
        in += 4;

        auto convert = [](int y, int u_val, int v_val) {
            int c = y - 16;
            int d = u_val - 128;
            int e = v_val - 128;

            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;

            return std::array<std::uint8_t, 3>{
                frame_clamp_to_u8(r),
                frame_clamp_to_u8(g),
                frame_clamp_to_u8(b)
            };
        };

        auto rgb0 = convert(y0, u, v);
        auto rgb1 = convert(y1, u, v);

        rgb[out++] = rgb0[0];
        rgb[out++] = rgb0[1];
        rgb[out++] = rgb0[2];

        rgb[out++] = rgb1[0];
        rgb[out++] = rgb1[1];
        rgb[out++] = rgb1[2];
    }
}

static void save_frame_to_files(
    const Frame& frame,
    const std::string& output_dir,
    int save_index
) {
    if (frame.data.empty()) {
        throw std::runtime_error("Cannot save empty frame");
    }

    if (frame.width == 0 || frame.height == 0 || frame.pixel_format == 0) {
        throw std::runtime_error("Cannot save frame with unknown format");
    }

    std::filesystem::create_directories(output_dir);

    std::ostringstream base;
    base << output_dir << "/pipeline_frame_"
         << std::setw(6) << std::setfill('0') << save_index
         << "_seq_" << frame.sequence;

    const std::string fourcc = frame_fourcc_to_string(frame.pixel_format);
    const std::string raw_path = base.str() + "." + fourcc;

    {
        std::ofstream raw(raw_path, std::ios::binary);
        if (!raw) {
            throw std::runtime_error("Failed to open raw output file: " + raw_path);
        }

        raw.write(
            reinterpret_cast<const char*>(frame.data.data()),
            static_cast<std::streamsize>(frame.data.size())
        );

        if (!raw) {
            throw std::runtime_error("Failed to write raw output file: " + raw_path);
        }
    }

    std::cout << "[SAVER] saved raw: " << raw_path
              << " bytes=" << frame.data.size() << "\n";

    if (frame.pixel_format == V4L2_PIX_FMT_YUYV) {
        const std::size_t expected_yuyv_size =
            static_cast<std::size_t>(frame.width) * frame.height * 2;

        if (frame.data.size() < expected_yuyv_size) {
            throw std::runtime_error("YUYV frame data is smaller than expected");
        }

        std::vector<std::uint8_t> rgb;
        frame_yuyv_to_rgb(frame.data.data(), rgb, frame.width, frame.height);

        const std::string ppm_path = base.str() + ".ppm";
        std::ofstream ppm(ppm_path, std::ios::binary);
        if (!ppm) {
            throw std::runtime_error("Failed to open ppm output file: " + ppm_path);
        }

        ppm << "P6\n" << frame.width << " " << frame.height << "\n255\n";
        ppm.write(
            reinterpret_cast<const char*>(rgb.data()),
            static_cast<std::streamsize>(rgb.size())
        );

        if (!ppm) {
            throw std::runtime_error("Failed to write ppm output file: " + ppm_path);
        }

        std::cout << "[SAVER] saved ppm: " << ppm_path
                  << " bytes=" << rgb.size() << "\n";
    } else {
        std::cout << "[SAVER] PPM conversion skipped for format "
                  << fourcc << "\n";
    }
}


static void prepare_output_directory(const std::string& output_dir) {
    if (output_dir.empty()) {
        throw std::invalid_argument("Output directory must not be empty");
    }

    std::filesystem::create_directories(output_dir);

    const auto probe_path =
        std::filesystem::path(output_dir) / ".v4l2_capture_write_test";

    {
        std::ofstream probe(probe_path, std::ios::binary | std::ios::trunc);
        if (!probe) {
            throw std::runtime_error("Output directory is not writable: " + output_dir);
        }

        probe << "test";
        if (!probe) {
            throw std::runtime_error("Failed to write test file in output directory: " + output_dir);
        }
    }

    std::error_code ec;
    std::filesystem::remove(probe_path, ec);
}

static void run_pipeline(
    CameraDevice& camera,
    int frame_count,
    int timeout_ms,
    int ring_capacity,
    int consumer_delay_ms,
    bool pipeline_save,
    int save_limit,
    const std::string& output_dir
) {
    if (frame_count <= 0) {
        throw std::runtime_error("Pipeline frame count must be positive");
    }

    if (ring_capacity <= 0) {
        throw std::runtime_error("Ring capacity must be positive");
    }

    RingBuffer<Frame> ring(static_cast<std::size_t>(ring_capacity));

    std::atomic<bool> producer_done{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<int> saved{0};
    std::atomic<std::uint64_t> consumed_bytes{0};

    std::mutex cv_mutex;
    std::condition_variable cv;

    std::exception_ptr producer_error = nullptr;
    std::exception_ptr consumer_error = nullptr;

    std::cout << "========== Pipeline Capture ==========\n";
    std::cout << "target frames      : " << frame_count << "\n";
    std::cout << "ring capacity      : " << ring_capacity << "\n";
    std::cout << "poll timeout       : " << timeout_ms << " ms\n";
    std::cout << "consumer delay     : " << consumer_delay_ms << " ms\n";
    std::cout << "pipeline save      : " << (pipeline_save ? "yes" : "no") << "\n";
    std::cout << "save limit         : " << save_limit << "\n";
    std::cout << "output dir         : " << output_dir << "\n";

    const auto start_time = std::chrono::steady_clock::now();

    std::thread consumer_thread([&]() {
        try {
            while (!producer_done.load() || !ring.empty()) {
                Frame frame;

                if (ring.try_pop_oldest(frame)) {
                    ++consumed;
                    consumed_bytes += frame.bytesused;

                    const int count = consumed.load();

                    if (pipeline_save && saved.load() < save_limit) {
                        const int save_index = saved.fetch_add(1);
                        if (save_index < save_limit) {
                            save_frame_to_files(frame, output_dir, save_index);
                        }
                    }

                    if (count == 1 || count == frame_count || count % 50 == 0) {
                        std::cout << "[CONSUMER] consumed "
                                  << count
                                  << " sequence=" << frame.sequence
                                  << " bytesused=" << frame.bytesused
                                  << "\n";
                    }

                    if (consumer_delay_ms > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(consumer_delay_ms)
                        );
                    }

                    continue;
                }

                std::unique_lock<std::mutex> lock(cv_mutex);
                cv.wait_for(lock, std::chrono::milliseconds(100));
            }
        } catch (...) {
            consumer_error = std::current_exception();
            stop_requested = true;
            cv.notify_all();
        }
    });

    std::thread producer_thread([&]() {
        try {
            for (int i = 0; i < frame_count && !stop_requested.load(); ++i) {
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
    std::cout << "saved frames         : " << saved.load() << "\n";
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
              << "  " << program << " --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --pipeline-frames 300 --ring-capacity 8 --timeout-ms 2000\n"
              << "  " << program << " --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --pipeline-frames 300 --ring-capacity 2 --consumer-delay-ms 50 --timeout-ms 2000\n"
              << "  " << program << " --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --pipeline-frames 300 --ring-capacity 8 --pipeline-save --save-limit 5 --output output/pipeline --timeout-ms 2000\n";
}

int main(int argc, char* argv[]) {
    std::string device = "/dev/video10";
    bool list_formats = false;
    bool capture_one = false;
    bool save_one = false;
    int capture_frames = 0;
    int pipeline_frames = 0;
    int ring_capacity = 8;
    int consumer_delay_ms = 0;
    bool pipeline_save = false;
    int save_limit = 5;
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
            } else if (arg == "--consumer-delay-ms" && i + 1 < argc) {
                consumer_delay_ms = parse_int_arg(argv[++i], arg);
            } else if (arg == "--pipeline-save") {
                pipeline_save = true;
            } else if (arg == "--save-limit" && i + 1 < argc) {
                save_limit = parse_int_arg(argv[++i], arg);
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
            if (save_one || (pipeline_frames > 0 && pipeline_save)) {
                prepare_output_directory(output_dir);
            }

            camera.start_streaming();

            if (save_one) {
                camera.capture_one_frame_to_files(timeout_ms, output_dir);
            } else if (capture_one) {
                camera.capture_one_frame(timeout_ms);
            } else if (capture_frames > 0) {
                camera.capture_frames(capture_frames, timeout_ms);
            } else {
                run_pipeline(
                    camera,
                    pipeline_frames,
                    timeout_ms,
                    ring_capacity,
                    consumer_delay_ms,
                    pipeline_save,
                    save_limit,
                    output_dir
                );
            }

            camera.stop_streaming();
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }

    return 0;
}
