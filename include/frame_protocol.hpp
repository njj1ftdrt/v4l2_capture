#pragma once

#include <linux/videodev2.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace frame_protocol {

constexpr std::uint32_t kMagic = 0x56345450;  // Existing v3 wire value.
constexpr std::uint16_t kVersion = 3;
constexpr std::size_t kWireHeaderSize = 44;
constexpr std::uint32_t kDefaultMaxPayloadBytes = 16u * 1024u * 1024u;
constexpr std::uint32_t kMaxDimension = 16384u;

// Host-side representation. It is never copied directly onto the wire.
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

using WireHeader = std::array<std::uint8_t, kWireHeaderSize>;

inline bool is_little_endian_host() noexcept {
    const std::uint16_t value = 0x1u;
    return *reinterpret_cast<const std::uint8_t*>(&value) == 0x1u;
}

// Kept for source compatibility with earlier call sites. The v3 codec now
// serializes every integer explicitly, so host endianness and struct padding
// are no longer protocol constraints.
inline void require_supported_host_layout() noexcept {}

inline void write_u16_le(
    WireHeader& out,
    std::size_t offset,
    std::uint16_t value
) noexcept {
    out[offset + 0] = static_cast<std::uint8_t>(value & 0xffu);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
}

inline void write_u32_le(
    WireHeader& out,
    std::size_t offset,
    std::uint32_t value
) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xffu);
    }
}

inline void write_u64_le(
    WireHeader& out,
    std::size_t offset,
    std::uint64_t value
) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xffu);
    }
}

inline std::uint16_t read_u16_le(
    const std::uint8_t* bytes,
    std::size_t offset
) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset + 0]) |
        (static_cast<std::uint16_t>(bytes[offset + 1]) << 8u)
    );
}

inline std::uint32_t read_u32_le(
    const std::uint8_t* bytes,
    std::size_t offset
) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8u);
    }
    return value;
}

inline std::uint64_t read_u64_le(
    const std::uint8_t* bytes,
    std::size_t offset
) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8u);
    }
    return value;
}

inline WireHeader serialize_header(const FrameHeader& header) noexcept {
    WireHeader wire{};
    write_u32_le(wire, 0, header.magic);
    write_u16_le(wire, 4, header.header_size);
    write_u16_le(wire, 6, header.version);
    write_u64_le(wire, 8, header.frame_id);
    write_u64_le(wire, 16, header.capture_timestamp_ns);
    write_u32_le(wire, 24, header.width);
    write_u32_le(wire, 28, header.height);
    write_u32_le(wire, 32, header.pixel_format);
    write_u32_le(wire, 36, header.payload_size);
    write_u32_le(wire, 40, header.payload_crc32);
    return wire;
}

inline bool deserialize_header(
    const void* data,
    std::size_t size,
    FrameHeader& header,
    std::string& reason
) {
    if (data == nullptr) {
        reason = "null wire header";
        return false;
    }
    if (size != kWireHeaderSize) {
        reason = "wire header size mismatch: got=" + std::to_string(size) +
                 ", expected=" + std::to_string(kWireHeaderSize);
        return false;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    FrameHeader decoded{};
    decoded.magic = read_u32_le(bytes, 0);
    decoded.header_size = read_u16_le(bytes, 4);
    decoded.version = read_u16_le(bytes, 6);
    decoded.frame_id = read_u64_le(bytes, 8);
    decoded.capture_timestamp_ns = read_u64_le(bytes, 16);
    decoded.width = read_u32_le(bytes, 24);
    decoded.height = read_u32_le(bytes, 28);
    decoded.pixel_format = read_u32_le(bytes, 32);
    decoded.payload_size = read_u32_le(bytes, 36);
    decoded.payload_crc32 = read_u32_le(bytes, 40);

    header = decoded;
    reason.clear();
    return true;
}

inline bool deserialize_header(
    const WireHeader& wire,
    FrameHeader& header,
    std::string& reason
) {
    return deserialize_header(wire.data(), wire.size(), header, reason);
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
    header.header_size = static_cast<std::uint16_t>(kWireHeaderSize);
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

    if (header.header_size != kWireHeaderSize) {
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
