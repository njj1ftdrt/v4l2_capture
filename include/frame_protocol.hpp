#pragma once

#include <linux/videodev2.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace frame_protocol {

constexpr std::uint32_t kMagic = 0x56345450;  // 'V4TP'
constexpr std::uint16_t kVersion = 3;
constexpr std::uint32_t kDefaultMaxPayloadBytes = 16u * 1024u * 1024u;
constexpr std::uint32_t kMaxDimension = 16384u;

#pragma pack(push, 1)
struct FrameHeader {
    std::uint32_t magic{0};
    std::uint16_t header_size{0};
    std::uint16_t version{0};
    std::uint64_t frame_id{0};
    std::uint64_t capture_timestamp_ns{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t pixel_format{0};
    std::uint32_t payload_size{0};
    std::uint32_t payload_crc32{0};
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 44, "Unexpected FrameHeader size");

inline bool is_little_endian_host() noexcept {
    const std::uint16_t value = 0x1u;
    return *reinterpret_cast<const std::uint8_t*>(&value) == 0x1u;
}

inline void require_supported_host_layout() {
    if (!is_little_endian_host()) {
        throw std::runtime_error(
            "FrameHeader v3 currently requires a little-endian host"
        );
    }
}

inline std::uint32_t compute_crc32(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = 0xFFFFFFFFu;

    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

inline FrameHeader make_header(
    std::uint64_t frame_id,
    std::uint64_t capture_timestamp_ns,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t pixel_format,
    std::uint32_t payload_size,
    std::uint32_t payload_crc32
) {
    FrameHeader header{};
    header.magic = kMagic;
    header.header_size = static_cast<std::uint16_t>(sizeof(FrameHeader));
    header.version = kVersion;
    header.frame_id = frame_id;
    header.capture_timestamp_ns = capture_timestamp_ns;
    header.width = width;
    header.height = height;
    header.pixel_format = pixel_format;
    header.payload_size = payload_size;
    header.payload_crc32 = payload_crc32;
    return header;
}

inline bool validate_header(
    const FrameHeader& header,
    std::uint32_t max_payload_bytes,
    std::string& reason
) {
    if (max_payload_bytes == 0) {
        reason = "receiver max payload must be positive";
        return false;
    }

    if (header.magic != kMagic) {
        reason = "invalid magic";
        return false;
    }

    if (header.header_size != sizeof(FrameHeader)) {
        reason = "unsupported header size: " + std::to_string(header.header_size);
        return false;
    }

    if (header.version != kVersion) {
        reason = "unsupported protocol version: " + std::to_string(header.version);
        return false;
    }

    if (header.capture_timestamp_ns == 0) {
        reason = "zero capture timestamp";
        return false;
    }

    if (header.width == 0 || header.height == 0) {
        reason = "zero width or height";
        return false;
    }

    if (header.width > kMaxDimension || header.height > kMaxDimension) {
        reason = "frame dimension exceeds limit";
        return false;
    }

    if (header.pixel_format == 0) {
        reason = "missing pixel format";
        return false;
    }

    if (header.payload_size == 0) {
        reason = "zero payload size";
        return false;
    }

    if (header.payload_size > max_payload_bytes) {
        reason = "payload size " + std::to_string(header.payload_size) +
                 " exceeds receiver limit " + std::to_string(max_payload_bytes);
        return false;
    }

    if (header.pixel_format == V4L2_PIX_FMT_YUYV) {
        if ((header.width % 2u) != 0u) {
            reason = "YUYV width must be even";
            return false;
        }

        const std::uint64_t expected =
            static_cast<std::uint64_t>(header.width) *
            static_cast<std::uint64_t>(header.height) * 2u;

        if (expected > std::numeric_limits<std::uint32_t>::max()) {
            reason = "YUYV payload size overflows protocol field";
            return false;
        }

        if (header.payload_size != static_cast<std::uint32_t>(expected)) {
            reason = "YUYV payload size mismatch: got=" +
                     std::to_string(header.payload_size) +
                     ", expected=" + std::to_string(expected);
            return false;
        }
    }

    reason.clear();
    return true;
}

inline bool is_valid_header(const FrameHeader& header) {
    std::string ignored;
    return validate_header(
        header,
        std::numeric_limits<std::uint32_t>::max(),
        ignored
    );
}

}  // namespace frame_protocol
