#include "font_resource_integrity.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace zevryon::text {
namespace {

constexpr std::uint32_t kHeadTag = sfnt_tag('h', 'e', 'a', 'd');
constexpr std::size_t kHeadChecksumAdjustmentOffset = 8U;
constexpr std::size_t kHeadChecksumAdjustmentSize = 4U;
constexpr std::size_t kHeadMinimumChecksumLength =
    kHeadChecksumAdjustmentOffset + kHeadChecksumAdjustmentSize;

void clear_error(SfntIntegrityError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    SfntIntegrityErrorKind kind,
    std::size_t byte_offset,
    std::uint32_t table_index,
    std::uint32_t table_tag,
    std::uint32_t expected,
    std::uint32_t actual,
    const char* message,
    SfntIntegrityError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->byte_offset = byte_offset;
        error->table_index = table_index;
        error->table_tag = table_tag;
        error->expected = expected;
        error->actual = actual;
        error->message = message;
    }
    return false;
}

bool checked_align_four(std::size_t value, std::size_t* output) noexcept {
    if (output == nullptr || value > std::numeric_limits<std::size_t>::max() - 3U) {
        return false;
    }
    *output = (value + 3U) & ~std::size_t{3U};
    return true;
}

bool checked_add_u64(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr || right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *output = left + right;
    return true;
}

} // namespace

const char* sfnt_integrity_error_kind_name(
    SfntIntegrityErrorKind kind) noexcept {
    switch (kind) {
    case SfntIntegrityErrorKind::None:
        return "none";
    case SfntIntegrityErrorKind::InvalidView:
        return "invalid_view";
    case SfntIntegrityErrorKind::MissingHeadTable:
        return "missing_head_table";
    case SfntIntegrityErrorKind::InvalidHeadTable:
        return "invalid_head_table";
    case SfntIntegrityErrorKind::MisalignedDirectory:
        return "misaligned_directory";
    case SfntIntegrityErrorKind::MisalignedTable:
        return "misaligned_table";
    case SfntIntegrityErrorKind::PaddingOutOfBounds:
        return "padding_out_of_bounds";
    case SfntIntegrityErrorKind::NonZeroPadding:
        return "non_zero_padding";
    case SfntIntegrityErrorKind::TableChecksumMismatch:
        return "table_checksum_mismatch";
    case SfntIntegrityErrorKind::WholeFontChecksumMismatch:
        return "whole_font_checksum_mismatch";
    case SfntIntegrityErrorKind::ArithmeticOverflow:
        return "arithmetic_overflow";
    }
    return "unknown";
}

std::uint32_t calculate_sfnt_checksum(
    std::span<const std::byte> bytes,
    std::size_t zero_offset,
    std::size_t zero_length) noexcept {
    std::size_t padded_size = bytes.size();
    if (!checked_align_four(bytes.size(), &padded_size)) {
        return 0U;
    }

    std::uint32_t sum = 0U;
    for (std::size_t word_offset = 0U; word_offset < padded_size;
         word_offset += 4U) {
        std::uint32_t word = 0U;
        for (std::size_t byte_index = 0U; byte_index < 4U; ++byte_index) {
            const std::size_t offset = word_offset + byte_index;
            unsigned char value = 0U;
            if (offset < bytes.size()) {
                const bool zeroed =
                    zero_length != 0U && offset >= zero_offset &&
                    offset - zero_offset < zero_length;
                if (!zeroed) {
                    value = std::to_integer<unsigned char>(bytes[offset]);
                }
            }
            word |= static_cast<std::uint32_t>(value)
                    << static_cast<unsigned int>((3U - byte_index) * 8U);
        }
        sum += word;
    }
    return sum;
}

bool verify_sfnt_integrity(
    const SfntResourceView& view,
    const SfntIntegrityOptions& options,
    SfntIntegrityStats* stats,
    SfntIntegrityError* error) noexcept {
    if (stats != nullptr) {
        *stats = {};
    }
    clear_error(error);
    if (!view.valid()) {
        return fail(SfntIntegrityErrorKind::InvalidView, 0U, 0U, 0U, 0U, 0U,
                    "integrity verification requires a valid sfnt view", error);
    }

    const std::span<const std::byte> source = view.bytes();
    if (options.require_four_byte_alignment && (view.face_offset() & 3U) != 0U) {
        return fail(SfntIntegrityErrorKind::MisalignedDirectory,
                    view.face_offset(), 0U, 0U, 0U, view.face_offset() & 3U,
                    "selected sfnt directory is not four-byte aligned", error);
    }

    SfntTableRecord head_record;
    const bool head_present = view.find_table(kHeadTag, &head_record);
    if (stats != nullptr) {
        stats->head_table_present = head_present;
    }
    if (options.require_head_table && !head_present) {
        return fail(SfntIntegrityErrorKind::MissingHeadTable, view.face_offset(),
                    0U, kHeadTag, 1U, 0U,
                    "required head table is missing", error);
    }
    if (head_present &&
        (options.verify_table_checksums ||
         options.verify_single_font_checksum_adjustment) &&
        head_record.length < kHeadMinimumChecksumLength) {
        return fail(SfntIntegrityErrorKind::InvalidHeadTable, head_record.offset,
                    0U, kHeadTag,
                    static_cast<std::uint32_t>(kHeadMinimumChecksumLength),
                    head_record.length,
                    "head table is too short for checkSumAdjustment", error);
    }

    std::uint64_t payload_bytes_checked = 0U;
    std::uint64_t padding_bytes_checked = 0U;
    std::uint32_t aligned_tables = 0U;
    std::uint32_t checksums_verified = 0U;

    for (std::uint16_t index = 0U; index < view.table_count(); ++index) {
        SfntTableRecord record;
        if (!view.table_record(index, &record)) {
            return fail(SfntIntegrityErrorKind::InvalidView, view.face_offset(),
                        index, 0U, 0U, 0U,
                        "validated sfnt table record became unavailable", error);
        }

        if ((record.offset & 3U) == 0U) {
            ++aligned_tables;
        } else if (options.require_four_byte_alignment) {
            return fail(SfntIntegrityErrorKind::MisalignedTable, record.offset,
                        index, record.tag, 0U, record.offset & 3U,
                        "sfnt table is not four-byte aligned", error);
        }

        const std::span<const std::byte> table = view.table_data(record);
        if (record.length != 0U && table.size() != record.length) {
            return fail(SfntIntegrityErrorKind::InvalidView, record.offset,
                        index, record.tag, record.length,
                        static_cast<std::uint32_t>(table.size()),
                        "validated sfnt table slice became unavailable", error);
        }

        std::uint64_t next_payload = 0U;
        if (!checked_add_u64(payload_bytes_checked, record.length,
                             &next_payload)) {
            return fail(SfntIntegrityErrorKind::ArithmeticOverflow,
                        record.offset, index, record.tag, 0U, 0U,
                        "integrity payload byte counter overflow", error);
        }
        payload_bytes_checked = next_payload;

        std::size_t padded_length = 0U;
        if (!checked_align_four(record.length, &padded_length)) {
            return fail(SfntIntegrityErrorKind::ArithmeticOverflow,
                        record.offset, index, record.tag, 0U, 0U,
                        "sfnt padded table length overflow", error);
        }
        const std::size_t padding_length = padded_length - record.length;
        if (options.require_zero_padding && padding_length != 0U) {
            const std::size_t padding_offset =
                static_cast<std::size_t>(record.offset) + record.length;
            if (padding_offset > source.size() ||
                padding_length > source.size() - padding_offset) {
                return fail(SfntIntegrityErrorKind::PaddingOutOfBounds,
                            padding_offset, index, record.tag,
                            static_cast<std::uint32_t>(padding_length), 0U,
                            "sfnt table padding escapes the resource", error);
            }
            for (std::size_t offset = 0U; offset < padding_length; ++offset) {
                const unsigned char value =
                    std::to_integer<unsigned char>(source[padding_offset + offset]);
                if (value != 0U) {
                    return fail(SfntIntegrityErrorKind::NonZeroPadding,
                                padding_offset + offset, index, record.tag, 0U,
                                value,
                                "sfnt table padding contains a non-zero byte",
                                error);
                }
            }
            std::uint64_t next_padding = 0U;
            if (!checked_add_u64(padding_bytes_checked, padding_length,
                                 &next_padding)) {
                return fail(SfntIntegrityErrorKind::ArithmeticOverflow,
                            padding_offset, index, record.tag, 0U, 0U,
                            "integrity padding byte counter overflow", error);
            }
            padding_bytes_checked = next_padding;
        }

        if (options.verify_table_checksums) {
            const bool is_head = record.tag == kHeadTag;
            const std::uint32_t actual = is_head
                ? calculate_sfnt_checksum(
                      table,
                      kHeadChecksumAdjustmentOffset,
                      kHeadChecksumAdjustmentSize)
                : calculate_sfnt_checksum(table);
            if (actual != record.checksum) {
                return fail(SfntIntegrityErrorKind::TableChecksumMismatch,
                            record.offset, index, record.tag, record.checksum,
                            actual, "sfnt table checksum does not match directory",
                            error);
            }
            ++checksums_verified;
        }
    }

    bool whole_font_verified = false;
    bool whole_font_ignored = false;
    if (options.verify_single_font_checksum_adjustment) {
        if (view.container_kind() == SfntContainerKind::Collection) {
            whole_font_ignored = true;
        } else {
            if (!head_present) {
                return fail(SfntIntegrityErrorKind::MissingHeadTable,
                            view.face_offset(), 0U, kHeadTag, 1U, 0U,
                            "whole-font checksum requires a head table", error);
            }
            const std::uint32_t actual = calculate_sfnt_checksum(source);
            if (actual != kSfntWholeFontChecksum) {
                return fail(
                    SfntIntegrityErrorKind::WholeFontChecksumMismatch,
                    static_cast<std::size_t>(head_record.offset) +
                        kHeadChecksumAdjustmentOffset,
                    0U, kHeadTag, kSfntWholeFontChecksum, actual,
                    "single-font checksumAdjustment does not balance the font",
                    error);
            }
            whole_font_verified = true;
        }
    }

    if (stats != nullptr) {
        stats->tables_seen = view.table_count();
        stats->aligned_tables = aligned_tables;
        stats->checksums_verified = checksums_verified;
        stats->payload_bytes_checked = payload_bytes_checked;
        stats->padding_bytes_checked = padding_bytes_checked;
        stats->head_table_present = head_present;
        stats->whole_font_checksum_verified = whole_font_verified;
        stats->whole_font_checksum_ignored_for_collection = whole_font_ignored;
    }
    return true;
}

} // namespace zevryon::text
