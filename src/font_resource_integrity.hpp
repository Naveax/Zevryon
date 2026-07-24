#pragma once

#include "font_resource_sfnt.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace zevryon::text {

enum class SfntIntegrityErrorKind : std::uint8_t {
    None = 0,
    InvalidView,
    MissingHeadTable,
    InvalidHeadTable,
    MisalignedDirectory,
    MisalignedTable,
    PaddingOutOfBounds,
    NonZeroPadding,
    TableChecksumMismatch,
    WholeFontChecksumMismatch,
    ArithmeticOverflow
};

struct SfntIntegrityError {
    SfntIntegrityErrorKind kind{SfntIntegrityErrorKind::None};
    std::size_t byte_offset{0};
    std::uint32_t table_index{0};
    std::uint32_t table_tag{0};
    std::uint32_t expected{0};
    std::uint32_t actual{0};
    const char* message{nullptr};
};

struct SfntIntegrityOptions {
    bool require_head_table{true};
    bool require_four_byte_alignment{true};
    bool require_zero_padding{true};
    bool verify_table_checksums{true};
    bool verify_single_font_checksum_adjustment{true};
};

struct SfntIntegrityStats {
    std::uint32_t tables_seen{0};
    std::uint32_t aligned_tables{0};
    std::uint32_t checksums_verified{0};
    std::uint64_t payload_bytes_checked{0};
    std::uint64_t padding_bytes_checked{0};
    bool head_table_present{false};
    bool whole_font_checksum_verified{false};
    bool whole_font_checksum_ignored_for_collection{false};
};

inline constexpr std::uint32_t kSfntWholeFontChecksum = 0xB1B0AFBAU;

const char* sfnt_integrity_error_kind_name(
    SfntIntegrityErrorKind kind) noexcept;

// Computes the OpenType uint32 big-endian checksum, padding a final partial
// word with zero bytes. Bytes in [zero_offset, zero_offset + zero_length) are
// treated as zero. Passing zero_length == 0 disables the zero range.
std::uint32_t calculate_sfnt_checksum(
    std::span<const std::byte> bytes,
    std::size_t zero_offset = 0U,
    std::size_t zero_length = 0U) noexcept;

// Applies structural-integrity policy to an already validated non-owning sfnt
// resource view. This function allocates no memory and never changes the view.
bool verify_sfnt_integrity(
    const SfntResourceView& view,
    const SfntIntegrityOptions& options,
    SfntIntegrityStats* stats,
    SfntIntegrityError* error) noexcept;

} // namespace zevryon::text
