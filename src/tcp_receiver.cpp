#include "frame_protocol.hpp"
#include "stats_json.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
        std::size_t pos = 0;
        const long value = std::stol(text, &pos, 10);
        if (pos != std::strlen(text) || value < 0 || value > 1000000) {
            throw std::runtime_error("out of range");
        }
        return static_cast<int>(value);
    } catch (...) {
        throw std::invalid_argument("Invalid integer for " + name + ": " + text);
    }
}

std::uint32_t parse_u32_arg(const char* text, const std::string& name) {
    try {
        std::size_t pos = 0;
        const unsigned long long value = std::stoull(text, &pos, 10);
        if (pos != std::strlen(text) || value == 0 ||
            value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("out of range");
        }
        return static_cast<std::uint32_t>(value);
    } catch (...) {
        throw std::invalid_argument("Invalid unsigned integer for " + name + ": " + text);
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

    const std::string path =
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
              << "  " << program
              << " --port 9000 --output output/tcp_recv"
              << " --stats-output output/stats/receiver_stats.json"
              << " --max-payload-bytes 16777216"
              << " --max-sessions 2\n";
}

}  // namespace

int main(int argc, char** argv) {
    int listen_fd = -1;
    int client_fd = -1;

    try {
        int port = 9000;
        std::string output_dir = "output/tcp_recv";
        int max_frames = 0;
        int max_sessions = 1;
        std::string stats_output = "output/stats/receiver_stats.json";
        std::uint32_t max_payload_bytes = frame_protocol::kDefaultMaxPayloadBytes;

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
            } else if (arg == "--max-sessions" && i + 1 < argc) {
                max_sessions = parse_int_arg(argv[++i], arg);
            } else if (arg == "--stats-output" && i + 1 < argc) {
                stats_output = argv[++i];
            } else if (arg == "--max-payload-bytes" && i + 1 < argc) {
                max_payload_bytes = parse_u32_arg(argv[++i], arg);
            } else {
                throw std::invalid_argument("Unknown or incomplete argument: " + arg);
            }
        }

        if (port <= 0 || port > 65535) {
            throw std::invalid_argument("--port must be between 1 and 65535");
        }
        if (max_sessions <= 0) {
            throw std::invalid_argument("--max-sessions must be positive");
        }

        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
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
            throw std::runtime_error(
                std::string("bind failed: ") + std::strerror(errno)
            );
        }

        if (listen(listen_fd, max_sessions) < 0) {
            throw std::runtime_error(
                std::string("listen failed: ") + std::strerror(errno)
            );
        }

        std::cout << "[INFO] tcp_receiver listening on port " << port << "\n";
        std::cout << "[INFO] output dir: " << output_dir << "\n";
        std::cout << "[INFO] stats output: " << stats_output << "\n";
        std::cout << "[INFO] max payload bytes: " << max_payload_bytes << "\n";
        std::cout << "[INFO] max sessions: " << max_sessions << "\n";

        std::uint64_t received_frames = 0;
        std::uint64_t received_bytes = 0;
        std::uint64_t crc_errors = 0;
        std::uint64_t header_errors = 0;
        std::uint64_t rejected_frames = 0;
        std::uint64_t accepted_sessions = 0;
        std::uint64_t completed_sessions = 0;
        std::uint64_t peer_disconnects = 0;
        std::string last_error;

        bool fatal_protocol_error = false;

        while (accepted_sessions < static_cast<std::uint64_t>(max_sessions) &&
               (max_frames <= 0 || static_cast<int>(received_frames) < max_frames)) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);

            client_fd = accept(
                listen_fd,
                reinterpret_cast<sockaddr*>(&client_addr),
                &client_len
            );

            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(
                    std::string("accept failed: ") + std::strerror(errno)
                );
            }

            ++accepted_sessions;
            const std::uint64_t session_id = accepted_sessions;
            std::uint64_t session_frames = 0;
            bool session_ok = true;

            std::cout << "[INFO] client connected session=" << session_id << "\n";

            while (max_frames <= 0 || static_cast<int>(received_frames) < max_frames) {
                frame_protocol::FrameHeader header{};

                if (!read_exact(client_fd, &header, sizeof(header))) {
                    ++peer_disconnects;
                    std::cout << "[INFO] peer closed connection session="
                              << session_id
                              << " session_frames=" << session_frames
                              << "\n";
                    break;
                }

                std::string header_error;
                if (!frame_protocol::validate_header(header, max_payload_bytes, header_error)) {
                    ++header_errors;
                    ++rejected_frames;
                    last_error = "invalid frame header: " + header_error;
                    std::cerr << "[ERROR] " << last_error
                              << " frame_id=" << header.frame_id
                              << " payload_size=" << header.payload_size
                              << " session=" << session_id
                              << "\n";
                    session_ok = false;
                    fatal_protocol_error = true;
                    break;
                }

                // Allocate only after validating payload_size and format-specific invariants.
                std::vector<std::uint8_t> payload(header.payload_size);

                if (!read_exact(client_fd, payload.data(), payload.size())) {
                    ++peer_disconnects;
                    last_error = "peer closed while reading payload for frame_id=" +
                                 std::to_string(header.frame_id);
                    std::cout << "[WARN] " << last_error
                              << " session=" << session_id
                              << "\n";
                    session_ok = false;
                    break;
                }

                const std::uint32_t actual_crc32 = frame_protocol::compute_crc32(
                    payload.data(),
                    payload.size()
                );

                if (actual_crc32 != header.payload_crc32) {
                    ++crc_errors;
                    ++rejected_frames;
                    last_error = "payload crc mismatch for frame_id=" +
                                 std::to_string(header.frame_id);
                    std::cerr << "[ERROR] payload crc mismatch"
                              << " frame_id=" << header.frame_id
                              << " expected=0x" << std::hex << header.payload_crc32
                              << " actual=0x" << actual_crc32 << std::dec
                              << " session=" << session_id
                              << "\n";
                    session_ok = false;
                    fatal_protocol_error = true;
                    break;
                }

                ++received_frames;
                ++session_frames;
                received_bytes += payload.size();

                std::cout << "[RECV] frame=" << received_frames
                          << " frame_id=" << header.frame_id
                          << " session=" << session_id
                          << " size=" << header.width << "x" << header.height
                          << " format=" << fourcc_to_string(header.pixel_format)
                          << " payload=" << header.payload_size
                          << " crc32=0x" << std::hex << header.payload_crc32 << std::dec
                          << "\n";

                save_payload(header, payload, output_dir, received_frames);
            }

            close_fd(client_fd);
            client_fd = -1;

            if (session_ok) {
                ++completed_sessions;
            }

            std::cout << "[INFO] session closed session=" << session_id
                      << " session_frames=" << session_frames
                      << " total_frames=" << received_frames
                      << "\n";

            if (fatal_protocol_error) {
                break;
            }
        }

        std::cout << "========== TCP Receiver Statistics ==========\n";
        std::cout << "received frames    : " << received_frames << "\n";
        std::cout << "received bytes     : " << received_bytes << "\n";
        std::cout << "crc errors         : " << crc_errors << "\n";
        std::cout << "header errors      : " << header_errors << "\n";
        std::cout << "rejected frames    : " << rejected_frames << "\n";
        std::cout << "accepted sessions  : " << accepted_sessions << "\n";
        std::cout << "completed sessions : " << completed_sessions << "\n";
        std::cout << "peer disconnects   : " << peer_disconnects << "\n";
        if (!last_error.empty()) {
            std::cout << "last error         : " << last_error << "\n";
        }
        std::cout << "=============================================\n";

        ReceiverStatsSnapshot snapshot{};
        snapshot.received_frames = received_frames;
        snapshot.received_bytes = received_bytes;
        snapshot.crc_errors = crc_errors;
        snapshot.header_errors = header_errors;
        snapshot.rejected_frames = rejected_frames;
        snapshot.saved_files = received_frames;
        snapshot.accepted_sessions = accepted_sessions;
        snapshot.completed_sessions = completed_sessions;
        snapshot.peer_disconnects = peer_disconnects;
        snapshot.last_error = last_error;
        write_receiver_stats_json(stats_output, snapshot);
        std::cout << "[INFO] wrote machine-readable stats to " << stats_output << "\n";

        close_fd(listen_fd);
        listen_fd = -1;

        if (header_errors > 0) {
            return 3;
        }
        if (crc_errors > 0) {
            return 2;
        }
        return 0;
    } catch (const std::exception& e) {
        close_fd(client_fd);
        close_fd(listen_fd);
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }
}
