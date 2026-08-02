#include "frame_protocol.hpp"

#include <linux/videodev2.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect_valid(
    const frame_protocol::FrameHeader& header,
    std::uint32_t max_payload_bytes,
    const char* test_name
) {
    std::string reason;
    if (!frame_protocol::validate_header(header, max_payload_bytes, reason)) {
        std::cerr << "[FAIL] " << test_name << ": " << reason << "\n";
        std::exit(1);
    }
}

void expect_invalid(
    const frame_protocol::FrameHeader& header,
    std::uint32_t max_payload_bytes,
    const std::string& expected_reason_part,
    const char* test_name
) {
    std::string reason;
    if (frame_protocol::validate_header(header, max_payload_bytes, reason)) {
        std::cerr << "[FAIL] " << test_name << ": header unexpectedly accepted\n";
        std::exit(1);
    }

    if (reason.find(expected_reason_part) == std::string::npos) {
        std::cerr << "[FAIL] " << test_name
                  << ": reason='" << reason
                  << "' expected to contain '" << expected_reason_part << "'\n";
        std::exit(1);
    }
}

frame_protocol::FrameHeader make_valid_yuyv_header() {
    return frame_protocol::make_header(
        1,
        123456789,
        640,
        360,
        V4L2_PIX_FMT_YUYV,
        640u * 360u * 2u,
        0x12345678u
    );
}

}  // namespace

int main() {
    if (!frame_protocol::is_little_endian_host()) {
        std::cerr << "[FAIL] test host must be little-endian\n";
        return 1;
    }
    const auto valid = make_valid_yuyv_header();
    expect_valid(
        valid,
        frame_protocol::kDefaultMaxPayloadBytes,
        "valid YUYV header"
    );

    {
        auto header = valid;
        header.magic ^= 0x1u;
        expect_invalid(header, frame_protocol::kDefaultMaxPayloadBytes, "magic", "bad magic");
    }

    {
        auto header = valid;
        ++header.version;
        expect_invalid(header, frame_protocol::kDefaultMaxPayloadBytes, "version", "bad version");
    }

    {
        auto header = valid;
        header.capture_timestamp_ns = 0;
        expect_invalid(header, frame_protocol::kDefaultMaxPayloadBytes, "timestamp", "zero capture timestamp");
    }

    {
        auto header = valid;
        header.header_size = 1;
        expect_invalid(header, frame_protocol::kDefaultMaxPayloadBytes, "header size", "bad header size");
    }

    {
        auto header = valid;
        header.width = 0;
        expect_invalid(header, frame_protocol::kDefaultMaxPayloadBytes, "zero width", "zero width");
    }

    {
        auto header = valid;
        header.width = frame_protocol::kMaxDimension + 1u;
        expect_invalid(header, frame_protocol::kDefaultMaxPayloadBytes, "dimension", "oversized dimension");
    }

    {
        auto header = valid;
        header.payload_size = frame_protocol::kDefaultMaxPayloadBytes + 1u;
        expect_invalid(header, frame_protocol::kDefaultMaxPayloadBytes, "exceeds receiver limit", "oversized payload");
    }

    {
        auto header = valid;
        header.width = 641;
        header.payload_size = 641u * 360u * 2u;
        expect_invalid(header, frame_protocol::kDefaultMaxPayloadBytes, "width must be even", "odd YUYV width");
    }

    {
        auto header = valid;
        header.payload_size -= 2u;
        expect_invalid(header, frame_protocol::kDefaultMaxPayloadBytes, "payload size mismatch", "YUYV size mismatch");
    }

    std::cout << "Frame protocol validation tests passed.\n";
    return 0;
}
