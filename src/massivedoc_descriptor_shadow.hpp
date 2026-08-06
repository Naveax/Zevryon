#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace zevryon::massivedoc {

enum class MassiveDocDescriptorShadowOperation : std::uint32_t {
    None = 0,
    RecordEncode = 1,
    RecordDecode = 2,
    ChunkEncode = 3,
    ChunkDecode = 4,
};

struct MassiveDocDescriptorShadowSnapshot {
    std::uint64_t record_encode_checks{0};
    std::uint64_t record_decode_checks{0};
    std::uint64_t chunk_encode_checks{0};
    std::uint64_t chunk_decode_checks{0};
    std::uint64_t mismatches{0};
    MassiveDocDescriptorShadowOperation first_mismatch{
        MassiveDocDescriptorShadowOperation::None};
};

void reset_massivedoc_descriptor_shadow() noexcept;
MassiveDocDescriptorShadowSnapshot massivedoc_descriptor_shadow_snapshot() noexcept;

void verify_massivedoc_record_encoding(
    std::uint64_t logical_id,
    std::uint64_t first_chunk,
    std::uint64_t length,
    std::uint32_t chunk_count,
    std::uint32_t crc32,
    std::span<const std::byte> cpp_bytes) noexcept;

void verify_massivedoc_record_decoding(
    std::span<const std::byte> encoded,
    std::uint64_t logical_id,
    std::uint64_t first_chunk,
    std::uint64_t length,
    std::uint32_t chunk_count,
    std::uint32_t crc32) noexcept;

void verify_massivedoc_chunk_encoding(
    std::uint32_t segment_id,
    std::uint64_t offset,
    std::uint64_t length,
    std::span<const std::byte> cpp_bytes) noexcept;

void verify_massivedoc_chunk_decoding(
    std::span<const std::byte> encoded,
    std::uint32_t segment_id,
    std::uint64_t offset,
    std::uint64_t length) noexcept;

} // namespace zevryon::massivedoc
