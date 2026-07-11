#include "frame_protocol.hpp"
#include "stats_json.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void close_fd(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

bool read_exact(int fd, void* buffer, std::size_t size) {
    auto* out = static_cast<std::uint8_t*>(buffer);
    std::size_t total = 0;

    while (total < size) {
        const ssize_t n = recv(fd, out + total, size - total, 0);

        if (n == 0) {
            return false;
        }

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(
                std::string("recv failed: ") + std::strerror(errno)
            );
        }

        total += static_cast<std::size_t>(n);
    }

    return true;
}

int parse_int_arg(const char* text, const std::string& name) {
    try {
        return std::stoi(text);
    } catch (...) {
        throw std::invalid_argument("Invalid integer for " + name + ": " + text);
    }
}

std::string fourcc_to_string(std::uint32_t fourcc) {
    std::string s;
    s.push_back(static_cast<char>(fourcc & 0xff));
    s.push_back(static_cast<char>((fourcc >> 8) & 0xff));
    s.push_back(static_cast<char>((fourcc >> 16) & 0xff));
    s.push_back(static_cast<char>((fourcc >> 24) & 0xff));
    return s;
}

void save_payload(
    const frame_protocol::FrameHeader& header,
    const std::vector<std::uint8_t>& payload,
    const std::string& output_dir,
    std::uint64_t index
) {
    std::filesystem::create_directories(output_dir);

    const std::string fourcc = fourcc_to_string(header.pixel_format);

    std::string path =
        output_dir + "/recv_frame_" + std::to_string(index) +
        "_id_" + std::to_string(header.frame_id) + "." + fourcc;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Failed to open output file: " + path);
    }

    out.write(
        reinterpret_cast<const char*>(payload.data()),
        static_cast<std::streamsize>(payload.size())
    );

    if (!out) {
        throw std::runtime_error("Failed to write output file: " + path);
    }

    std::cout << "[RECV] saved " << path
              << " bytes=" << payload.size()
              << "\n";
}

void print_usage(const char* program) {
    std::cout << "Usage:\n"
              << "  " << program << " --port 9000 --output output/tcp_recv --stats-output output/stats/receiver_stats.json\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        int port = 9000;
        std::string output_dir = "output/tcp_recv";
        int max_frames = 0;
        std::string stats_output = "output/stats/receiver_stats.json";

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];

            if (arg == "--help") {
                print_usage(argv[0]);
                return 0;
            } else if (arg == "--port" && i + 1 < argc) {
                port = parse_int_arg(argv[++i], arg);
            } else if (arg == "--output" && i + 1 < argc) {
                output_dir = argv[++i];
            } else if (arg == "--max-frames" && i + 1 < argc) {
                max_frames = parse_int_arg(argv[++i], arg);
            } else if (arg == "--stats-output" && i + 1 < argc) {
                stats_output = argv[++i];
            } else {
                throw std::invalid_argument("Unknown or incomplete argument: " + arg);
            }
        }

        const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            throw std::runtime_error(
                std::string("socket failed: ") + std::strerror(errno)
            );
        }

        int yes = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<std::uint16_t>(port));

        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_fd(listen_fd);
            throw std::runtime_error(
                std::string("bind failed: ") + std::strerror(errno)
            );
        }

        if (listen(listen_fd, 1) < 0) {
            close_fd(listen_fd);
            throw std::runtime_error(
                std::string("listen failed: ") + std::strerror(errno)
            );
        }

        std::cout << "[INFO] tcp_receiver listening on port " << port << "\n";
        std::cout << "[INFO] output dir: " << output_dir << "\n";
        std::cout << "[INFO] stats output: " << stats_output << "\n";

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        const int client_fd = accept(
            listen_fd,
            reinterpret_cast<sockaddr*>(&client_addr),
            &client_len
        );

        if (client_fd < 0) {
            close_fd(listen_fd);
            throw std::runtime_error(
                std::string("accept failed: ") + std::strerror(errno)
            );
        }

        std::cout << "[INFO] client connected\n";

        std::uint64_t received_frames = 0;
        std::uint64_t received_bytes = 0;
        std::uint64_t crc_errors = 0;

        while (max_frames <= 0 || static_cast<int>(received_frames) < max_frames) {
            frame_protocol::FrameHeader header{};

            if (!read_exact(client_fd, &header, sizeof(header))) {
                std::cout << "[INFO] peer closed connection\n";
                break;
            }

            if (!frame_protocol::is_valid_header(header)) {
                std::cerr << "[ERROR] invalid frame header\n";
                break;
            }

            std::vector<std::uint8_t> payload(header.payload_size);

            if (!read_exact(client_fd, payload.data(), payload.size())) {
                std::cout << "[WARN] peer closed while reading payload\n";
                break;
            }

            const std::uint32_t actual_crc32 = frame_protocol::compute_crc32(
                payload.data(),
                payload.size()
            );

            if (actual_crc32 != header.payload_crc32) {
                ++crc_errors;
                std::cerr << "[ERROR] payload crc mismatch"
                          << " frame_id=" << header.frame_id
                          << " expected=0x" << std::hex << header.payload_crc32
                          << " actual=0x" << actual_crc32 << std::dec
                          << "\n";
                break;
            }

            ++received_frames;
            received_bytes += payload.size();

            std::cout << "[RECV] frame=" << received_frames
                      << " frame_id=" << header.frame_id
                      << " size=" << header.width << "x" << header.height
                      << " format=" << fourcc_to_string(header.pixel_format)
                      << " payload=" << header.payload_size
                      << " crc32=0x" << std::hex << header.payload_crc32 << std::dec
                      << "\n";

            save_payload(header, payload, output_dir, received_frames);
        }

        std::cout << "========== TCP Receiver Statistics ==========\n";
        std::cout << "received frames : " << received_frames << "\n";
        std::cout << "received bytes  : " << received_bytes << "\n";
        std::cout << "crc errors      : " << crc_errors << "\n";
        std::cout << "=============================================\n";

        ReceiverStatsSnapshot snapshot{};
        snapshot.received_frames = received_frames;
        snapshot.received_bytes = received_bytes;
        snapshot.crc_errors = crc_errors;
        snapshot.saved_files = received_frames;
        write_receiver_stats_json(stats_output, snapshot);
        std::cout << "[INFO] wrote machine-readable stats to " << stats_output << "\n";

        close_fd(client_fd);
        close_fd(listen_fd);
        return crc_errors == 0 ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }
}
