#include "app_config.hpp"
#include "camera_device.hpp"
#include "frame.hpp"
#include "frame_protocol.hpp"
#include "ring_buffer.hpp"

#include <linux/videodev2.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static void close_socket_fd(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

static std::uint64_t current_system_time_ns() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
    );
}

static int connect_to_tcp_receiver(const std::string& host, int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(
            std::string("socket failed: ") + std::strerror(errno)
        );
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close_socket_fd(fd);
        throw std::runtime_error("Invalid IPv4 address for --tcp-host: " + host);
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_socket_fd(fd);
        throw std::runtime_error(
            std::string("connect failed: ") + std::strerror(errno)
        );
    }

    return fd;
}

static void send_all_tcp(int fd, const void* data, std::size_t size) {
    const auto* ptr = static_cast<const std::uint8_t*>(data);
    std::size_t sent = 0;

    while (sent < size) {
        const ssize_t n = send(
            fd,
            ptr + sent,
            size - sent,
#ifdef MSG_NOSIGNAL
            MSG_NOSIGNAL
#else
            0
#endif
        );

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            throw std::runtime_error(
                std::string("send failed: ") + std::strerror(errno)
            );
        }

        if (n == 0) {
            throw std::runtime_error("send returned 0, peer may have closed connection");
        }

        sent += static_cast<std::size_t>(n);
    }
}

static std::uint64_t send_frame_over_tcp(int fd, const Frame& frame) {
    if (frame.data.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Frame payload is too large for FrameHeader payload_size");
    }

    const auto header = frame_protocol::make_header(
        static_cast<std::uint64_t>(frame.sequence),
        current_system_time_ns(),
        frame.width,
        frame.height,
        frame.pixel_format,
        static_cast<std::uint32_t>(frame.data.size())
    );

    send_all_tcp(fd, &header, sizeof(header));
    send_all_tcp(fd, frame.data.data(), frame.data.size());

    return static_cast<std::uint64_t>(sizeof(header)) + frame.data.size();
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


static std::size_t expected_frame_size_bytes(const Frame& frame) {
    if (frame.pixel_format == V4L2_PIX_FMT_YUYV) {
        return static_cast<std::size_t>(frame.width) *
               static_cast<std::size_t>(frame.height) * 2;
    }

    // MJPG is compressed, so bytesused changes from frame to frame.
    // Do not validate MJPG with width * height * 2.
    return 0;
}

static bool is_frame_usable(const Frame& frame, std::string& reason) {
    if (frame.data.empty()) {
        reason = "empty frame data";
        return false;
    }

    if (frame.width == 0 || frame.height == 0 || frame.pixel_format == 0) {
        reason = "unknown frame format or size";
        return false;
    }

    if (frame.pixel_format == V4L2_PIX_FMT_YUYV) {
        const std::size_t expected = expected_frame_size_bytes(frame);
        if (frame.data.size() < expected) {
            reason = "incomplete YUYV frame: got=" +
                     std::to_string(frame.data.size()) +
                     ", expected=" +
                     std::to_string(expected);
            return false;
        }
    }

    return true;
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
    const std::string& output_dir,
    const std::optional<std::string>& tcp_host,
    int tcp_port,
    int tcp_queue_capacity
) {
    if (frame_count <= 0) {
        throw std::runtime_error("Pipeline frame count must be positive");
    }

    if (ring_capacity <= 0) {
        throw std::runtime_error("Ring capacity must be positive");
    }

    if (tcp_queue_capacity <= 0) {
        throw std::runtime_error("TCP queue capacity must be positive");
    }

    const bool tcp_enabled = tcp_host.has_value() && tcp_port > 0;

    RingBuffer<Frame> ring(static_cast<std::size_t>(ring_capacity));
    RingBuffer<Frame> tcp_ring(static_cast<std::size_t>(tcp_queue_capacity));

    std::atomic<bool> producer_done{false};
    std::atomic<bool> consumer_done{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<int> invalid_frames{0};
    std::atomic<int> saved{0};
    std::atomic<int> tcp_enqueued_frames{0};
    std::atomic<int> tcp_sent_frames{0};
    std::atomic<int> tcp_send_errors{0};
    std::atomic<std::uint64_t> consumed_bytes{0};
    std::atomic<std::uint64_t> tcp_sent_bytes{0};
    std::string tcp_error_message;

    std::mutex cv_mutex;
    std::condition_variable cv;
    std::mutex tcp_cv_mutex;
    std::condition_variable tcp_cv;

    std::exception_ptr producer_error = nullptr;
    std::exception_ptr consumer_error = nullptr;
    std::exception_ptr tcp_thread_error = nullptr;

    std::cout << "========== Pipeline Capture ==========\n";
    std::cout << "target frames      : " << frame_count << "\n";
    std::cout << "ring capacity      : " << ring_capacity << "\n";
    std::cout << "poll timeout       : " << timeout_ms << " ms\n";
    std::cout << "consumer delay     : " << consumer_delay_ms << " ms\n";
    std::cout << "pipeline save      : " << (pipeline_save ? "yes" : "no") << "\n";
    std::cout << "save limit         : " << save_limit << "\n";
    std::cout << "output dir         : " << output_dir << "\n";
    std::cout << "tcp send           : " << (tcp_enabled ? "yes" : "no") << "\n";
    if (tcp_enabled) {
        std::cout << "tcp target         : " << *tcp_host << ":" << tcp_port << "\n";
        std::cout << "tcp queue capacity : " << tcp_queue_capacity << "\n";
    }

    const auto start_time = std::chrono::steady_clock::now();

    std::thread tcp_sender_thread;
    if (tcp_enabled) {
        tcp_sender_thread = std::thread([&]() {
            int tcp_fd = -1;

            try {
                try {
                    tcp_fd = connect_to_tcp_receiver(*tcp_host, tcp_port);
                    std::cout << "[TCP] connected to " << *tcp_host << ":" << tcp_port << "\n";
                } catch (const std::exception& e) {
                    ++tcp_send_errors;
                    tcp_error_message = std::string("connect to ") + *tcp_host + ":" +
                                        std::to_string(tcp_port) + " failed: " + e.what();
                    std::cerr << "[TCP][ERROR] " << tcp_error_message << "\n";
                    stop_requested = true;
                    cv.notify_all();
                    tcp_cv.notify_all();
                    return;
                }

                while (!consumer_done.load() || !tcp_ring.empty()) {
                    Frame frame;

                    if (tcp_ring.try_pop_oldest(frame)) {
                        try {
                            const std::uint64_t bytes = send_frame_over_tcp(tcp_fd, frame);
                            tcp_sent_bytes += bytes;
                            const int tcp_count = ++tcp_sent_frames;

                            if (tcp_count == 1 || tcp_count == frame_count || tcp_count % 50 == 0) {
                                std::cout << "[TCP] sent "
                                          << tcp_count
                                          << " frame_id=" << frame.sequence
                                          << " bytes=" << bytes
                                          << "\n";
                            }
                        } catch (const std::exception& e) {
                            ++tcp_send_errors;
                            tcp_error_message = "send frame_id=" +
                                                std::to_string(frame.sequence) +
                                                " failed after tcp_sent_frames=" +
                                                std::to_string(tcp_sent_frames.load()) +
                                                ": " + e.what();
                            std::cerr << "[TCP][ERROR] " << tcp_error_message << "\n";
                            stop_requested = true;
                            cv.notify_all();
                            tcp_cv.notify_all();
                            break;
                        }

                        continue;
                    }

                    std::unique_lock<std::mutex> lock(tcp_cv_mutex);
                    tcp_cv.wait_for(lock, std::chrono::milliseconds(100));
                }

                close_socket_fd(tcp_fd);
            } catch (...) {
                close_socket_fd(tcp_fd);
                tcp_thread_error = std::current_exception();
                stop_requested = true;
                cv.notify_all();
                tcp_cv.notify_all();
            }
        });
    }

    std::thread consumer_thread([&]() {
        try {
            while (!stop_requested.load() && (!producer_done.load() || !ring.empty())) {
                Frame frame;

                if (ring.try_pop_oldest(frame)) {
                    ++consumed;
                    consumed_bytes += frame.bytesused;

                    const int count = consumed.load();

                    std::string invalid_reason;
                    if (!is_frame_usable(frame, invalid_reason)) {
                        const int invalid_count = ++invalid_frames;

                        if (invalid_count <= 5 || invalid_count % 50 == 0) {
                            std::cout << "[WARN] skip invalid frame"
                                      << " sequence=" << frame.sequence
                                      << " bytesused=" << frame.bytesused
                                      << " data_size=" << frame.data.size()
                                      << " reason=" << invalid_reason
                                      << "\n";
                        }

                        continue;
                    }

                    if (pipeline_save && saved.load() < save_limit) {
                        const int save_index = saved.fetch_add(1);
                        if (save_index < save_limit) {
                            save_frame_to_files(frame, output_dir, save_index);
                        }
                    }

                    if (tcp_enabled) {
                        tcp_ring.push(std::move(frame));
                        const int queued = ++tcp_enqueued_frames;
                        tcp_cv.notify_one();

                        if (queued == 1 || queued == frame_count || queued % 50 == 0) {
                            std::cout << "[TCP_QUEUE] enqueued "
                                      << queued
                                      << " queue_size=" << tcp_ring.size()
                                      << " queue_dropped=" << tcp_ring.dropped_count()
                                      << "\n";
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

            consumer_done = true;
            tcp_cv.notify_all();
        } catch (...) {
            consumer_error = std::current_exception();
            consumer_done = true;
            stop_requested = true;
            cv.notify_all();
            tcp_cv.notify_all();
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
    if (tcp_sender_thread.joinable()) {
        tcp_sender_thread.join();
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
    std::cout << "tcp queued frames    : " << tcp_enqueued_frames.load() << "\n";
    std::cout << "tcp queue dropped    : " << tcp_ring.dropped_count() << "\n";
    std::cout << "tcp queue remaining  : " << tcp_ring.size() << "\n";
    std::cout << "tcp sent frames      : " << tcp_sent_frames.load() << "\n";
    std::cout << "tcp sent bytes       : " << tcp_sent_bytes.load() << "\n";
    std::cout << "tcp send errors      : " << tcp_send_errors.load() << "\n";
    if (!tcp_error_message.empty()) {
        std::cout << "tcp last error       : " << tcp_error_message << "\n";
    }
    std::cout << "invalid frames       : " << invalid_frames.load() << "\n";
    std::cout << "consumed bytes       : " << consumed_bytes.load() << "\n";
    std::cout << "=========================================\n";
    std::cout.unsetf(std::ios::floatfield);

    if (producer_error) {
        std::rethrow_exception(producer_error);
    }

    if (consumer_error) {
        std::rethrow_exception(consumer_error);
    }

    if (tcp_thread_error) {
        std::rethrow_exception(tcp_thread_error);
    }

    if (tcp_send_errors.load() > 0) {
        throw std::runtime_error("TCP transmission failed: " + tcp_error_message);
    }
}

static void print_usage(const char* program) {
    std::cout << "Usage:\n"
              << "  " << program << " --config config/v4l2_tcp_pipeline.conf\n"
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
              << "  " << program << " --device /dev/video10 --width 640 --height 480 --format YUYV --mmap-buffers 4 --pipeline-frames 300 --ring-capacity 8 --pipeline-save --save-limit 5 --output output/pipeline --timeout-ms 2000\n"
              << "  " << program << " --device /dev/video0 --width 640 --height 360 --format YUYV --mmap-buffers 4 --pipeline-frames 30 --ring-capacity 8 --tcp-host 127.0.0.1 --tcp-port 9000 --tcp-queue-capacity 8 --timeout-ms 2000\n"
              << "  " << program << " --config config/v4l2_tcp_pipeline.conf --pipeline-frames 300\n";
}

int main(int argc, char* argv[]) {
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                return 0;
            }
        }

        const AppConfig config = load_app_config(argc, argv);
        print_app_config(config);

        CameraDevice camera(config.device);
        camera.open_device();
        camera.query_capability();

        if (config.list_formats) {
            camera.list_formats();
        }

        if (config.width.has_value() && config.height.has_value() && config.pixel_format.has_value()) {
            camera.set_format(*config.width, *config.height, *config.pixel_format);
        }

        if (config.mmap_buffers.has_value()) {
            camera.init_mmap_buffers(*config.mmap_buffers);
        }

        const int capture_mode_count =
            (config.capture_one ? 1 : 0) +
            (config.save_one ? 1 : 0) +
            (config.capture_frames > 0 ? 1 : 0) +
            (config.pipeline_frames > 0 ? 1 : 0);

        if (capture_mode_count > 0) {
            if (config.save_one || (config.pipeline_frames > 0 && config.pipeline_save)) {
                prepare_output_directory(config.output_dir);
            }

            camera.start_streaming();

            if (config.save_one) {
                camera.capture_one_frame_to_files(config.timeout_ms, config.output_dir);
            } else if (config.capture_one) {
                camera.capture_one_frame(config.timeout_ms);
            } else if (config.capture_frames > 0) {
                camera.capture_frames(config.capture_frames, config.timeout_ms);
            } else {
                run_pipeline(
                    camera,
                    config.pipeline_frames,
                    config.timeout_ms,
                    config.ring_capacity,
                    config.consumer_delay_ms,
                    config.pipeline_save,
                    config.save_limit,
                    config.output_dir,
                    config.tcp_host,
                    config.tcp_port,
                    config.tcp_queue_capacity
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
