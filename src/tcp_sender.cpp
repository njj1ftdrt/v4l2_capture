#include "frame_protocol.hpp"

#include <arpa/inet.h>
#include <linux/videodev2.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void close_fd(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

int parse_int_arg(const char* text, const std::string& name) {
    try {
        size_t pos = 0;
        const long value = std::stol(text, &pos, 10);
        if (pos != std::strlen(text) || value <= 0 || value > 1000000) {
            throw std::runtime_error("out of range");
        }
        return static_cast<int>(value);
    } catch (...) {
        throw std::invalid_argument("Invalid integer for " + name + ": " + text);
    }
}

std::uint64_t now_ns() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
    );
}

std::string fourcc_to_string(std::uint32_t fourcc) {
    std::string s;
    s.push_back(static_cast<char>(fourcc & 0xff));
    s.push_back(static_cast<char>((fourcc >> 8) & 0xff));
    s.push_back(static_cast<char>((fourcc >> 16) & 0xff));
    s.push_back(static_cast<char>((fourcc >> 24) & 0xff));
    return s;
}

std::uint32_t parse_format(const std::string& format) {
    if (format == "YUYV") {
        return V4L2_PIX_FMT_YUYV;
    }

    throw std::invalid_argument(
        "Unsupported --format " + format + ". Current tcp_sender test mode only supports YUYV"
    );
}

void send_all(int fd, const void* data, std::size_t size) {
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

std::vector<std::uint8_t> make_test_yuyv_payload(int width, int height, int frame_id) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("width and height must be positive");
    }

    if (width % 2 != 0) {
        throw std::invalid_argument("YUYV width must be even");
    }

    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 2
    );

    std::size_t offset = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; x += 2) {
            const std::uint8_t y0 = static_cast<std::uint8_t>((x + frame_id * 3) % 256);
            const std::uint8_t y1 = static_cast<std::uint8_t>((x + y + frame_id * 3) % 256);
            const std::uint8_t u = 128;
            const std::uint8_t v = 128;

            payload[offset++] = y0;
            payload[offset++] = u;
            payload[offset++] = y1;
            payload[offset++] = v;
        }
    }

    return payload;
}

int connect_to_server(const std::string& host, int port) {
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
        close_fd(fd);
        throw std::invalid_argument("Invalid IPv4 address for --host: " + host);
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_fd(fd);
        throw std::runtime_error(
            std::string("connect failed: ") + std::strerror(errno)
        );
    }

    return fd;
}

void apply_header_fault(
    frame_protocol::FrameHeader& header,
    const std::string& fault
) {
    if (fault == "bad-magic") {
        header.magic ^= 0x1u;
    } else if (fault == "bad-version") {
        ++header.version;
    } else if (fault == "bad-header-size") {
        header.header_size = 1;
    } else if (fault == "zero-width") {
        header.width = 0;
    } else if (fault == "oversized-payload") {
        header.payload_size = frame_protocol::kDefaultMaxPayloadBytes + 1u;
    } else if (fault == "yuyv-size-mismatch") {
        if (header.payload_size <= 2u) {
            throw std::runtime_error("payload too small for yuyv-size-mismatch fault");
        }
        header.payload_size -= 2u;
    } else {
        throw std::invalid_argument(
            "Unsupported --header-fault: " + fault +
            ". Expected bad-magic, bad-version, bad-header-size, zero-width, "
            "oversized-payload, or yuyv-size-mismatch"
        );
    }
}

void print_usage(const char* program) {
    std::cout << "Usage:\n"
              << "  " << program
              << " --host 127.0.0.1 --port 9000 --frames 3 --width 640 --height 360 --format YUYV [--corrupt-frame-id 2]\n"
              << "  " << program
              << " --host 127.0.0.1 --port 9000 --frames 1 --header-fault bad-magic\n"
              << "\n"
              << "Test-only options:\n"
              << "  --corrupt-frame-id N  Calculate the CRC first, then flip one payload byte for frame N.\n"
              << "  --header-fault TYPE   Send one malformed header and no payload.\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string host = "127.0.0.1";
        int port = 9000;
        int frames = 1;
        int width = 640;
        int height = 360;
        std::string format_text = "YUYV";
        int interval_ms = 33;
        int corrupt_frame_id = -1;
        std::string header_fault;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];

            if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                return 0;
            } else if (arg == "--host" && i + 1 < argc) {
                host = argv[++i];
            } else if (arg == "--port" && i + 1 < argc) {
                port = parse_int_arg(argv[++i], arg);
            } else if (arg == "--frames" && i + 1 < argc) {
                frames = parse_int_arg(argv[++i], arg);
            } else if (arg == "--width" && i + 1 < argc) {
                width = parse_int_arg(argv[++i], arg);
            } else if (arg == "--height" && i + 1 < argc) {
                height = parse_int_arg(argv[++i], arg);
            } else if (arg == "--format" && i + 1 < argc) {
                format_text = argv[++i];
            } else if (arg == "--interval-ms" && i + 1 < argc) {
                interval_ms = parse_int_arg(argv[++i], arg);
            } else if (arg == "--corrupt-frame-id" && i + 1 < argc) {
                corrupt_frame_id = parse_int_arg(argv[++i], arg);
            } else if (arg == "--header-fault" && i + 1 < argc) {
                header_fault = argv[++i];
            } else {
                throw std::invalid_argument("Unknown or incomplete argument: " + arg);
            }
        }

        const std::uint32_t pixel_format = parse_format(format_text);

        if (!header_fault.empty() && frames != 1) {
            throw std::invalid_argument("--header-fault requires --frames 1");
        }

        std::cout << "[INFO] tcp_sender connecting to "
                  << host << ":" << port << "\n";
        std::cout << "[INFO] test frame: "
                  << width << "x" << height
                  << " format=" << format_text
                  << " frames=" << frames << "\n";

        const int fd = connect_to_server(host, port);
        std::cout << "[INFO] connected\n";

        std::uint64_t sent_bytes = 0;

        for (int i = 0; i < frames; ++i) {
            std::vector<std::uint8_t> payload = make_test_yuyv_payload(width, height, i);

            const std::uint32_t payload_crc32 = frame_protocol::compute_crc32(
                payload.data(),
                payload.size()
            );

            auto header = frame_protocol::make_header(
                static_cast<std::uint64_t>(i),
                now_ns(),
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height),
                pixel_format,
                static_cast<std::uint32_t>(payload.size()),
                payload_crc32
            );

            if (!header_fault.empty()) {
                apply_header_fault(header, header_fault);
                send_all(fd, &header, sizeof(header));
                sent_bytes += sizeof(header);
                std::cout << "[TEST] sent malformed header"
                          << " fault=" << header_fault
                          << " frame_id=" << i
                          << " payload_size=" << header.payload_size
                          << "\n";
                break;
            }

            if (i == corrupt_frame_id) {
                if (payload.empty()) {
                    throw std::runtime_error("cannot corrupt an empty payload");
                }
                payload[payload.size() / 2] ^= 0x01u;
                std::cout << "[TEST] intentionally corrupted frame_id=" << i
                          << " after CRC calculation\n";
            }

            send_all(fd, &header, sizeof(header));
            send_all(fd, payload.data(), payload.size());

            sent_bytes += sizeof(header) + payload.size();

            std::cout << "[SEND] frame_id=" << i
                      << " size=" << width << "x" << height
                      << " format=" << fourcc_to_string(pixel_format)
                      << " payload=" << payload.size()
                      << " crc32=0x" << std::hex << payload_crc32 << std::dec
                      << "\n";

            if (i + 1 < frames && interval_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            }
        }

        close_fd(fd);

        std::cout << "========== TCP Sender Statistics ==========" << "\n";
        if (header_fault.empty()) {
            std::cout << "sent frames : " << frames << "\n";
        } else {
            std::cout << "sent malformed headers : 1\n";
        }
        std::cout << "sent bytes  : " << sent_bytes << "\n";
        std::cout << "===========================================" << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }
}
