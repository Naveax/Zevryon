#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace logical_node_source_v2_reparent_detail {

inline std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0U;
    for (unsigned index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

inline std::uint64_t u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

inline void put_u32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
    }
}

inline void put_u64(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
    }
}

inline std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= static_cast<std::uint32_t>(byte);
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

inline bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

} // namespace logical_node_source_v2_reparent_detail

// Tree-builder staging primitive. Apply only to a private, completed ZVNSRC01
// v2 file before its final publication/rename. Parent ordering and frame CRC are
// revalidated and rewritten on every mutation.
inline bool reparent_logical_node_source_v2_staging_file(
    const std::filesystem::path& source_path,
    std::uint64_t child_ordinal,
    std::uint64_t new_parent_ordinal,
    std::string* error) {
    using namespace logical_node_source_v2_reparent_detail;
    constexpr std::size_t kHeaderBytes = 112U;
    constexpr std::uint32_t kMinFrameBytes = 68U;
    constexpr std::uint32_t kMaxFrameBytes = 16U * 1024U * 1024U;
    constexpr std::array<std::uint8_t, 8> kMagic{
        'Z', 'V', 'N', 'S', 'R', 'C', '0', '1'};

    if (error == nullptr) {
        return false;
    }
    error->clear();
    std::fstream stream(source_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        return fail(error, "cannot open logical node source v2 staging file for reparent");
    }

    std::array<std::uint8_t, kHeaderBytes> header{};
    stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!stream || !std::equal(kMagic.begin(), kMagic.end(), header.begin()) ||
        u32(header, 8U) != 2U || u32(header, 12U) != kHeaderBytes ||
        crc32(std::span<const std::uint8_t>(header.data(), 108U)) != u32(header, 108U)) {
        return fail(error, "logical node source v2 staging header is invalid");
    }

    const std::uint64_t node_count = u64(header, 16U);
    if (child_ordinal == 0U || child_ordinal >= node_count ||
        new_parent_ordinal >= child_ordinal) {
        return fail(error, "logical node source v2 reparent ordering is invalid");
    }

    for (std::uint64_t ordinal = 0U; ordinal < node_count; ++ordinal) {
        const std::streampos frame_start = stream.tellg();
        std::array<std::uint8_t, 4> size_bytes{};
        stream.read(reinterpret_cast<char*>(size_bytes.data()), 4);
        if (!stream) {
            return fail(error, "logical node source v2 staging frame is truncated");
        }
        const std::uint32_t frame_bytes = u32(size_bytes, 0U);
        if (frame_bytes < kMinFrameBytes || frame_bytes > kMaxFrameBytes) {
            return fail(error, "logical node source v2 staging frame size is invalid");
        }
        if (ordinal != child_ordinal) {
            stream.seekg(static_cast<std::streamoff>(frame_bytes - 4U), std::ios::cur);
            if (!stream) {
                return fail(error, "logical node source v2 staging frame scan failed");
            }
            continue;
        }

        std::vector<std::uint8_t> frame(frame_bytes);
        std::copy(size_bytes.begin(), size_bytes.end(), frame.begin());
        stream.read(
            reinterpret_cast<char*>(frame.data() + 4U),
            static_cast<std::streamsize>(frame.size() - 4U));
        if (!stream) {
            return fail(error, "logical node source v2 staging target frame is truncated");
        }
        const std::size_t crc_offset = frame.size() - 4U;
        if (crc32(std::span<const std::uint8_t>(frame.data(), crc_offset)) !=
                u32(frame, crc_offset) ||
            u64(frame, 8U) != child_ordinal + 1U) {
            return fail(error, "logical node source v2 staging target frame integrity failed");
        }

        put_u64(frame, 40U, new_parent_ordinal);
        put_u32(frame, crc_offset,
                crc32(std::span<const std::uint8_t>(frame.data(), crc_offset)));
        stream.clear();
        stream.seekp(frame_start, std::ios::beg);
        stream.write(
            reinterpret_cast<const char*>(frame.data()),
            static_cast<std::streamsize>(frame.size()));
        stream.flush();
        return stream
            ? true
            : fail(error, "logical node source v2 staging reparent write failed");
    }
    return fail(error, "logical node source v2 staging child frame was not found");
}

} // namespace zevryon::massivedoc
