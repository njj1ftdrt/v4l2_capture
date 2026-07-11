#pragma once

#include <cstddef>
#include <cstdint>

namespace frame_protocol {

constexpr std::uint32_t kMagic = 0x56345450;  // 'V4TP'
constexpr std::uint16_t kVersion = 2;

#pragma pack(push, 1)
struct FrameHeader {
    std::uint32_t magic{0};
    std::uint16_t header_size{0};
    std::uint16_t version{0};
    std::uint64_t frame_id{0};
    std::uint64_t timestamp_ns{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t pixel_format{0};
    std::uint32_t payload_size{0};
    std::uint32_t payload_crc32{0};
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 44, "Unexpected FrameHeader size");

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
    std::uint64_t timestamp_ns,
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
    header.timestamp_ns = timestamp_ns;
    header.width = width;
    header.height = height;
    header.pixel_format = pixel_format;
    header.payload_size = payload_size;
    header.payload_crc32 = payload_crc32;
    return header;
}

inline bool is_valid_header(const FrameHeader& header) {
    return header.magic == kMagic &&
           header.header_size == sizeof(FrameHeader) &&
           header.version == kVersion &&
           header.width > 0 &&
           header.height > 0 &&
           header.pixel_format != 0 &&
           header.payload_size > 0;
}

}  // namespace frame_protocol
