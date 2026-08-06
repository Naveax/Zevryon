#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace zevryon::massivedoc {

inline constexpr std::size_t kMassiveDocRecordDescriptorBytes = 32U;
inline constexpr std::size_t kMassiveDocChunkDescriptorBytes = 24U;

enum class MassiveDocDescriptorShadowOperation : std::uint32_t {
    None = 0,
    RecordEncode = 1,
    RecordDecode = 2,
    ChunkEncode = 3,
    ChunkDecode = 4,
};

enum class MassiveDocDescriptorBackend : std::uint32_t {
    Cpp = 0,
    Rust = 1,
};

struct MassiveDocRecordDescriptorValue {
    std::uint64_t logical_id{0};
    std::uint64_t first_chunk{0};
    std::uint64_t length{0};
    std::uint32_t chunk_count{0};
    std::uint32_t crc32{0};

    friend bool operator==(
        const MassiveDocRecordDescriptorValue&,
        const MassiveDocRecordDescriptorValue&) = default;
};

struct MassiveDocChunkDescriptorValue {
    std::uint32_t segment_id{0};
    std::uint64_t offset{0};
    std::uint64_t length{0};

    friend bool operator==(
        const MassiveDocChunkDescriptorValue&,
        const MassiveDocChunkDescriptorValue&) = default;
};

struct MassiveDocDescriptorShadowSnapshot {
    std::uint64_t record_encode_checks{0};
    std::uint64_t record_decode_checks{0};
    std::uint64_t chunk_encode_checks{0};
    std::uint64_t chunk_decode_checks{0};
    std::uint64_t mismatches{0};
    MassiveDocDescriptorShadowOperation first_mismatch{
        MassiveDocDescriptorShadowOperation::None};
    MassiveDocDescriptorBackend authoritative_backend{
        MassiveDocDescriptorBackend::Cpp};
    MassiveDocDescriptorBackend verification_backend{
        MassiveDocDescriptorBackend::Rust};
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

#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE)
std::array<std::byte, kMassiveDocRecordDescriptorBytes>
encode_massivedoc_record_authoritative(
    const MassiveDocRecordDescriptorValue& descriptor,
    std::span<const std::byte> cpp_reverse_shadow_bytes) noexcept;

std::optional<MassiveDocRecordDescriptorValue>
decode_massivedoc_record_authoritative(
    std::span<const std::byte> encoded,
    const std::optional<MassiveDocRecordDescriptorValue>& cpp_reverse_shadow) noexcept;

std::array<std::byte, kMassiveDocChunkDescriptorBytes>
encode_massivedoc_chunk_authoritative(
    const MassiveDocChunkDescriptorValue& descriptor,
    std::span<const std::byte> cpp_reverse_shadow_bytes) noexcept;

std::optional<MassiveDocChunkDescriptorValue>
decode_massivedoc_chunk_authoritative(
    std::span<const std::byte> encoded,
    const std::optional<MassiveDocChunkDescriptorValue>& cpp_reverse_shadow) noexcept;
#endif

#if defined(ZEVRYON_MASSIVEDOC_CODEC_AUTHORITY_TEST_HOOKS)
void set_massivedoc_cpp_reverse_shadow_fault(
    MassiveDocDescriptorShadowOperation operation) noexcept;
void clear_massivedoc_cpp_reverse_shadow_fault() noexcept;
#endif

} // namespace zevryon::massivedoc
