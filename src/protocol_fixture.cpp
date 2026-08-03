#include "frame_protocol.hpp"

#include <linux/videodev2.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kFixtureWidth = 8;
constexpr std::uint32_t kFixtureHeight = 4;
constexpr std::uint64_t kFixtureTimestampBase = 1700000000000000000ull;
constexpr std::uint64_t kFixtureTimestampStep = 33333333ull;

struct Options {
    std::string write_path;
    std::string verify_path;
    std::string stats_output;
    std::uint64_t frames{32};
};

std::uint64_t parse_positive_u64(const char* text, const std::string& name) {
    try {
        std::size_t pos = 0;
        const unsigned long long value = std::stoull(text, &pos, 10);
        if (pos != std::strlen(text) || value == 0 || value > 1000000ull) {
            throw std::runtime_error("out of range");
        }
        return static_cast<std::uint64_t>(value);
    } catch (...) {
        throw std::invalid_argument("Invalid positive integer for " + name + ": " + text);
    }
}

void print_usage(const char* program) {
    std::cout
        << "Usage:\n"
        << "  " << program << " --write FILE [--frames 32] [--stats-output FILE]\n"
        << "  " << program << " --verify FILE [--frames 32] [--stats-output FILE]\n";
}

Options parse_options(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--write" && i + 1 < argc) {
            options.write_path = argv[++i];
        } else if (arg == "--verify" && i + 1 < argc) {
            options.verify_path = argv[++i];
        } else if (arg == "--frames" && i + 1 < argc) {
            options.frames = parse_positive_u64(argv[++i], arg);
        } else if (arg == "--stats-output" && i + 1 < argc) {
            options.stats_output = argv[++i];
        } else {
            throw std::invalid_argument("Unknown or incomplete argument: " + arg);
        }
    }

    const bool writing = !options.write_path.empty();
    const bool verifying = !options.verify_path.empty();
    if (writing == verifying) {
        throw std::invalid_argument("Specify exactly one of --write or --verify");
    }

    return options;
}

std::vector<std::uint8_t> make_fixture_payload(std::uint64_t frame_id) {
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(kFixtureWidth) * kFixtureHeight * 2u
    );

    std::size_t offset = 0;
    for (std::uint32_t y = 0; y < kFixtureHeight; ++y) {
        for (std::uint32_t x = 0; x < kFixtureWidth; x += 2) {
            const auto seed = static_cast<std::uint32_t>(frame_id & 0xffffffffu);
            const std::uint8_t y0 = static_cast<std::uint8_t>((x + y * 17u + seed * 3u) & 0xffu);
            const std::uint8_t y1 = static_cast<std::uint8_t>((x + y * 29u + seed * 5u + 1u) & 0xffu);
            const std::uint8_t u = static_cast<std::uint8_t>((96u + y * 7u + seed) & 0xffu);
            const std::uint8_t v = static_cast<std::uint8_t>((160u + x * 5u + seed * 2u) & 0xffu);

            payload[offset++] = y0;
            payload[offset++] = u;
            payload[offset++] = y1;
            payload[offset++] = v;
        }
    }

    return payload;
}

std::uint64_t fixture_timestamp(std::uint64_t frame_id) {
    return kFixtureTimestampBase + frame_id * kFixtureTimestampStep;
}

std::string json_escape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

void write_stats(
    const std::string& path,
    const std::string& mode,
    const std::string& fixture_path,
    std::uint64_t frames,
    std::uint64_t bytes,
    bool verified
) {
    if (path.empty()) {
        return;
    }

    const std::filesystem::path stats_path(path);
    if (stats_path.has_parent_path()) {
        std::filesystem::create_directories(stats_path.parent_path());
    }

    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Failed to open stats output: " + path);
    }

    out << "{\n"
        << "  \"mode\": \"" << json_escape(mode) << "\",\n"
        << "  \"fixture_path\": \"" << json_escape(fixture_path) << "\",\n"
        << "  \"protocol_version\": " << frame_protocol::kVersion << ",\n"
        << "  \"wire_header_size\": " << frame_protocol::kWireHeaderSize << ",\n"
        << "  \"frames\": " << frames << ",\n"
        << "  \"bytes\": " << bytes << ",\n"
        << "  \"verified\": " << (verified ? "true" : "false") << "\n"
        << "}\n";
}

void write_fixture(const Options& options) {
    const std::filesystem::path output_path(options.write_path);
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ofstream out(options.write_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to open fixture for writing: " + options.write_path);
    }

    std::uint64_t bytes_written = 0;
    for (std::uint64_t frame_id = 0; frame_id < options.frames; ++frame_id) {
        const auto payload = make_fixture_payload(frame_id);
        const auto header = frame_protocol::make_header(
            frame_id,
            fixture_timestamp(frame_id),
            kFixtureWidth,
            kFixtureHeight,
            V4L2_PIX_FMT_YUYV,
            static_cast<std::uint32_t>(payload.size()),
            frame_protocol::compute_crc32(payload.data(), payload.size())
        );
        const auto wire = frame_protocol::serialize_header(header);

        out.write(
            reinterpret_cast<const char*>(wire.data()),
            static_cast<std::streamsize>(wire.size())
        );
        out.write(
            reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size())
        );
        if (!out) {
            throw std::runtime_error("Failed while writing fixture: " + options.write_path);
        }
        bytes_written += wire.size() + payload.size();
    }

    out.close();
    write_stats(
        options.stats_output,
        "write",
        options.write_path,
        options.frames,
        bytes_written,
        true
    );

    std::cout << "[PASS] fixture_write"
              << " frames=" << options.frames
              << " bytes=" << bytes_written
              << " header_bytes=" << frame_protocol::kWireHeaderSize
              << " path=" << options.write_path
              << "\n";
}

void verify_fixture(const Options& options) {
    std::ifstream in(options.verify_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open fixture for verification: " + options.verify_path);
    }

    std::uint64_t verified_frames = 0;
    std::uint64_t verified_bytes = 0;

    while (true) {
        frame_protocol::WireHeader wire{};
        in.read(
            reinterpret_cast<char*>(wire.data()),
            static_cast<std::streamsize>(wire.size())
        );
        const std::streamsize header_bytes = in.gcount();
        if (header_bytes == 0 && in.eof()) {
            break;
        }
        if (header_bytes != static_cast<std::streamsize>(wire.size())) {
            throw std::runtime_error(
                "Truncated wire header at frame " + std::to_string(verified_frames)
            );
        }

        frame_protocol::FrameHeader header{};
        std::string reason;
        if (!frame_protocol::deserialize_header(wire, header, reason)) {
            throw std::runtime_error(
                "Header decode failed at frame " + std::to_string(verified_frames) +
                ": " + reason
            );
        }
        if (!frame_protocol::validate_header(
                header,
                frame_protocol::kDefaultMaxPayloadBytes,
                reason
            )) {
            throw std::runtime_error(
                "Header validation failed at frame " + std::to_string(verified_frames) +
                ": " + reason
            );
        }

        if (header.frame_id != verified_frames ||
            header.capture_timestamp_ns != fixture_timestamp(verified_frames) ||
            header.width != kFixtureWidth ||
            header.height != kFixtureHeight ||
            header.pixel_format != V4L2_PIX_FMT_YUYV) {
            throw std::runtime_error(
                "Deterministic header fields mismatch at frame " +
                std::to_string(verified_frames)
            );
        }

        std::vector<std::uint8_t> payload(header.payload_size);
        in.read(
            reinterpret_cast<char*>(payload.data()),
            static_cast<std::streamsize>(payload.size())
        );
        if (in.gcount() != static_cast<std::streamsize>(payload.size())) {
            throw std::runtime_error(
                "Truncated payload at frame " + std::to_string(verified_frames)
            );
        }

        const auto actual_crc = frame_protocol::compute_crc32(
            payload.data(),
            payload.size()
        );
        if (actual_crc != header.payload_crc32) {
            throw std::runtime_error(
                "CRC mismatch at frame " + std::to_string(verified_frames)
            );
        }

        const auto expected_payload = make_fixture_payload(verified_frames);
        if (payload != expected_payload) {
            throw std::runtime_error(
                "Deterministic payload mismatch at frame " +
                std::to_string(verified_frames)
            );
        }

        ++verified_frames;
        verified_bytes += wire.size() + payload.size();
    }

    if (verified_frames != options.frames) {
        throw std::runtime_error(
            "Fixture frame count mismatch: got=" + std::to_string(verified_frames) +
            ", expected=" + std::to_string(options.frames)
        );
    }

    write_stats(
        options.stats_output,
        "verify",
        options.verify_path,
        verified_frames,
        verified_bytes,
        true
    );

    std::cout << "[PASS] fixture_verify"
              << " frames=" << verified_frames
              << " bytes=" << verified_bytes
              << " crc_errors=0 header_errors=0 payload_mismatches=0"
              << " path=" << options.verify_path
              << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (!options.write_path.empty()) {
            write_fixture(options);
        } else {
            verify_fixture(options);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }
}
