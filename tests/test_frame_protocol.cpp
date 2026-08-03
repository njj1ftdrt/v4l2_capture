#include "frame_protocol.hpp"

#include <linux/videodev2.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void fail(const std::string& message) {
    std::cerr << "[FAIL] " << message << "\n";
    std::exit(1);
}

void expect_valid(
    const frame_protocol::FrameHeader& header,
    std::uint32_t max_payload_bytes,
    const char* test_name
) {
    std::string reason;
    if (!frame_protocol::validate_header(header, max_payload_bytes, reason)) {
        fail(std::string(test_name) + ": " + reason);
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
        fail(std::string(test_name) + ": header unexpectedly accepted");
    }

    if (reason.find(expected_reason_part) == std::string::npos) {
        fail(
            std::string(test_name) + ": reason='" + reason +
            "' expected to contain '" + expected_reason_part + "'"
        );
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

void expect_same_header(
    const frame_protocol::FrameHeader& actual,
    const frame_protocol::FrameHeader& expected
) {
    if (actual.magic != expected.magic ||
        actual.header_size != expected.header_size ||
        actual.version != expected.version ||
        actual.frame_id != expected.frame_id ||
        actual.capture_timestamp_ns != expected.capture_timestamp_ns ||
        actual.width != expected.width ||
        actual.height != expected.height ||
        actual.pixel_format != expected.pixel_format ||
        actual.payload_size != expected.payload_size ||
        actual.payload_crc32 != expected.payload_crc32) {
        fail("serialized header round-trip mismatch");
    }
}

void test_golden_wire_bytes() {
    frame_protocol::FrameHeader header{};
    header.magic = frame_protocol::kMagic;
    header.header_size = static_cast<std::uint16_t>(frame_protocol::kWireHeaderSize);
    header.version = frame_protocol::kVersion;
    header.frame_id = 0x0102030405060708ull;
    header.capture_timestamp_ns = 0x1112131415161718ull;
    header.width = 0x21222324u;
    header.height = 0x31323334u;
    header.pixel_format = 0x41424344u;
    header.payload_size = 0x51525354u;
    header.payload_crc32 = 0x61626364u;

    const frame_protocol::WireHeader expected{
        0x50, 0x54, 0x34, 0x56,
        0x2c, 0x00,
        0x03, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
        0x24, 0x23, 0x22, 0x21,
        0x34, 0x33, 0x32, 0x31,
        0x44, 0x43, 0x42, 0x41,
        0x54, 0x53, 0x52, 0x51,
        0x64, 0x63, 0x62, 0x61
    };

    const auto actual = frame_protocol::serialize_header(header);
    if (actual != expected) {
        fail("wire header bytes changed from the v3 golden fixture");
    }

    frame_protocol::FrameHeader decoded{};
    std::string reason;
    if (!frame_protocol::deserialize_header(actual, decoded, reason)) {
        fail("golden header decode failed: " + reason);
    }
    expect_same_header(decoded, header);

    if (frame_protocol::deserialize_header(
            actual.data(),
            actual.size() - 1u,
            decoded,
            reason
        )) {
        fail("truncated wire header unexpectedly decoded");
    }
}

}  // namespace

int main() {
    test_golden_wire_bytes();

    const auto valid = make_valid_yuyv_header();
    const auto wire = frame_protocol::serialize_header(valid);
    frame_protocol::FrameHeader round_trip{};
    std::string decode_reason;
    if (!frame_protocol::deserialize_header(wire, round_trip, decode_reason)) {
        fail("valid header decode failed: " + decode_reason);
    }
    expect_same_header(round_trip, valid);

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

    std::cout << "Frame protocol codec and validation tests passed.\n";
    return 0;
}
