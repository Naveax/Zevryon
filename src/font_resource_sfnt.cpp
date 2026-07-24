#include "font_resource_sfnt.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace zevryon::text {
namespace {

constexpr std::uint32_t kTtcfTag = sfnt_tag('t', 't', 'c', 'f');
constexpr std::uint32_t kOttoTag = sfnt_tag('O', 'T', 'T', 'O');
constexpr std::uint32_t kDsigTag = sfnt_tag('D', 'S', 'I', 'G');
constexpr std::uint32_t kTrueTypeVersion = 0x00010000U;
constexpr std::uint32_t kTtcVersion1 = 0x00010000U;
constexpr std::uint32_t kTtcVersion2 = 0x00020000U;
constexpr std::uint32_t kMaxCollectionFaces = 65535U;
constexpr std::uint16_t kMaxTablesPerFace = 1024U;
constexpr std::size_t kSfntDirectoryHeaderSize = 12U;
constexpr std::size_t kTableRecordSize = 16U;
constexpr std::size_t kTtcHeaderPrefixSize = 12U;
constexpr std::size_t kTtcVersion2SuffixSize = 12U;

void clear_error(SfntParseError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    SfntParseErrorKind kind,
    std::size_t byte_offset,
    std::uint32_t record_index,
    const char* message,
    SfntParseError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->byte_offset = byte_offset;
        error->record_index = record_index;
        error->message = message;
    }
    return false;
}

bool checked_add(std::size_t left, std::size_t right, std::size_t* output) noexcept {
    if (output == nullptr || right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    *output = left + right;
    return true;
}

bool checked_mul(std::size_t left, std::size_t right, std::size_t* output) noexcept {
    if (output == nullptr || (left != 0U && right > std::numeric_limits<std::size_t>::max() / left)) {
        return false;
    }
    *output = left * right;
    return true;
}

bool read_u16(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::uint16_t* output) noexcept {
    if (output == nullptr || offset > bytes.size() || bytes.size() - offset < 2U) {
        return false;
    }
    const auto first = static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset]));
    const auto second = static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset + 1U]));
    *output = static_cast<std::uint16_t>((first << 8U) | second);
    return true;
}

bool read_u32(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::uint32_t* output) noexcept {
    if (output == nullptr || offset > bytes.size() || bytes.size() - offset < 4U) {
        return false;
    }
    *output =
        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) << 24U) |
        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1U])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2U])) << 8U) |
        static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3U]));
    return true;
}

bool valid_tag(std::uint32_t tag) noexcept {
    for (unsigned int shift : {24U, 16U, 8U, 0U}) {
        const std::uint32_t value = (tag >> shift) & 0xffU;
        if (value < 0x20U || value > 0x7eU) {
            return false;
        }
    }
    return true;
}

bool intervals_overlap(
    std::uint32_t first_offset,
    std::uint32_t first_length,
    std::size_t second_offset,
    std::size_t second_length) noexcept {
    if (first_length == 0U || second_length == 0U) {
        return false;
    }
    const std::uint64_t first_begin = first_offset;
    const std::uint64_t first_end = first_begin + first_length;
    const std::uint64_t second_begin = second_offset;
    const std::uint64_t second_end = second_begin + second_length;
    return first_begin < second_end && second_begin < first_end;
}

bool read_record(
    std::span<const std::byte> bytes,
    std::uint32_t directory_offset,
    std::uint16_t index,
    SfntTableRecord* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    std::size_t record_delta = 0U;
    std::size_t record_offset = 0U;
    if (!checked_mul(static_cast<std::size_t>(index), kTableRecordSize, &record_delta) ||
        !checked_add(static_cast<std::size_t>(directory_offset) + kSfntDirectoryHeaderSize,
                     record_delta,
                     &record_offset)) {
        return false;
    }
    return read_u32(bytes, record_offset, &output->tag) &&
           read_u32(bytes, record_offset + 4U, &output->checksum) &&
           read_u32(bytes, record_offset + 8U, &output->offset) &&
           read_u32(bytes, record_offset + 12U, &output->length);
}

void expected_search_parameters(
    std::uint16_t table_count,
    std::uint16_t* search_range,
    std::uint16_t* entry_selector,
    std::uint16_t* range_shift) noexcept {
    std::uint16_t power = 1U;
    std::uint16_t selector = 0U;
    while (static_cast<std::uint32_t>(power) * 2U <= table_count) {
        power = static_cast<std::uint16_t>(power * 2U);
        selector = static_cast<std::uint16_t>(selector + 1U);
    }
    const std::uint16_t range = static_cast<std::uint16_t>(power * 16U);
    *search_range = range;
    *entry_selector = selector;
    *range_shift = static_cast<std::uint16_t>(table_count * 16U - range);
}

} // namespace

bool SfntResourceView::valid() const noexcept {
    return !bytes_.empty() && face_count_ != 0U && table_count_ != 0U;
}

SfntContainerKind SfntResourceView::container_kind() const noexcept {
    return container_kind_;
}

SfntFlavor SfntResourceView::flavor() const noexcept {
    return flavor_;
}

std::uint32_t SfntResourceView::face_count() const noexcept {
    return face_count_;
}

std::uint32_t SfntResourceView::face_index() const noexcept {
    return face_index_;
}

std::uint32_t SfntResourceView::face_offset() const noexcept {
    return directory_offset_;
}

std::uint16_t SfntResourceView::table_count() const noexcept {
    return table_count_;
}

std::span<const std::byte> SfntResourceView::bytes() const noexcept {
    return bytes_;
}

bool SfntResourceView::table_record(
    std::uint16_t index,
    SfntTableRecord* output) const noexcept {
    if (!valid() || output == nullptr || index >= table_count_) {
        return false;
    }
    return read_record(bytes_, directory_offset_, index, output);
}

bool SfntResourceView::find_table(
    std::uint32_t tag,
    SfntTableRecord* output) const noexcept {
    if (!valid() || output == nullptr) {
        return false;
    }
    std::uint16_t first = 0U;
    std::uint16_t count = table_count_;
    while (count != 0U) {
        const std::uint16_t step = static_cast<std::uint16_t>(count / 2U);
        const std::uint16_t index = static_cast<std::uint16_t>(first + step);
        SfntTableRecord record;
        if (!table_record(index, &record)) {
            return false;
        }
        if (record.tag < tag) {
            first = static_cast<std::uint16_t>(index + 1U);
            count = static_cast<std::uint16_t>(count - step - 1U);
        } else {
            count = step;
        }
    }
    if (first >= table_count_) {
        return false;
    }
    SfntTableRecord record;
    if (!table_record(first, &record) || record.tag != tag) {
        return false;
    }
    *output = record;
    return true;
}

std::span<const std::byte> SfntResourceView::table_data(
    const SfntTableRecord& record) const noexcept {
    const std::size_t offset = record.offset;
    const std::size_t length = record.length;
    if (!valid() || offset > bytes_.size() || length > bytes_.size() - offset) {
        return {};
    }
    return bytes_.subspan(offset, length);
}

const char* sfnt_parse_error_kind_name(SfntParseErrorKind kind) noexcept {
    switch (kind) {
    case SfntParseErrorKind::None:
        return "none";
    case SfntParseErrorKind::InvalidArgument:
        return "invalid_argument";
    case SfntParseErrorKind::TruncatedInput:
        return "truncated_input";
    case SfntParseErrorKind::UnsupportedContainerVersion:
        return "unsupported_container_version";
    case SfntParseErrorKind::InvalidCollection:
        return "invalid_collection";
    case SfntParseErrorKind::InvalidFaceIndex:
        return "invalid_face_index";
    case SfntParseErrorKind::UnsupportedSfntVersion:
        return "unsupported_sfnt_version";
    case SfntParseErrorKind::InvalidTableCount:
        return "invalid_table_count";
    case SfntParseErrorKind::InvalidTableTag:
        return "invalid_table_tag";
    case SfntParseErrorKind::UnsortedTableDirectory:
        return "unsorted_table_directory";
    case SfntParseErrorKind::DuplicateTableTag:
        return "duplicate_table_tag";
    case SfntParseErrorKind::InvalidTableRange:
        return "invalid_table_range";
    case SfntParseErrorKind::DirectoryOverlap:
        return "directory_overlap";
    case SfntParseErrorKind::TableOverlap:
        return "table_overlap";
    case SfntParseErrorKind::ArithmeticOverflow:
        return "arithmetic_overflow";
    }
    return "unknown";
}

bool open_sfnt_resource(
    std::span<const std::byte> bytes,
    std::uint32_t face_index,
    SfntResourceView* output,
    SfntParseStats* stats,
    SfntParseError* error) noexcept {
    if (output != nullptr) {
        *output = {};
    }
    if (stats != nullptr) {
        *stats = {};
    }
    clear_error(error);
    if (output == nullptr) {
        return fail(SfntParseErrorKind::InvalidArgument, 0U, 0U,
                    "output view is required", error);
    }
    if (bytes.size() < 4U) {
        return fail(SfntParseErrorKind::TruncatedInput, bytes.size(), 0U,
                    "font resource is shorter than a signature", error);
    }

    std::uint32_t signature = 0U;
    if (!read_u32(bytes, 0U, &signature)) {
        return fail(SfntParseErrorKind::TruncatedInput, 0U, 0U,
                    "font signature is truncated", error);
    }

    SfntContainerKind container_kind = SfntContainerKind::SingleFont;
    std::uint32_t face_count = 1U;
    std::uint32_t directory_offset = 0U;
    std::size_t collection_header_end = 0U;

    if (signature == kTtcfTag) {
        container_kind = SfntContainerKind::Collection;
        if (bytes.size() < kTtcHeaderPrefixSize) {
            return fail(SfntParseErrorKind::TruncatedInput, bytes.size(), 0U,
                        "TTC header is truncated", error);
        }
        std::uint32_t version = 0U;
        if (!read_u32(bytes, 4U, &version) || !read_u32(bytes, 8U, &face_count)) {
            return fail(SfntParseErrorKind::TruncatedInput, 4U, 0U,
                        "TTC version or face count is truncated", error);
        }
        if (version != kTtcVersion1 && version != kTtcVersion2) {
            return fail(SfntParseErrorKind::UnsupportedContainerVersion, 4U, 0U,
                        "unsupported TTC header version", error);
        }
        if (face_count == 0U || face_count > kMaxCollectionFaces) {
            return fail(SfntParseErrorKind::InvalidCollection, 8U, 0U,
                        "TTC face count is invalid", error);
        }
        std::size_t offsets_size = 0U;
        if (!checked_mul(static_cast<std::size_t>(face_count), 4U, &offsets_size) ||
            !checked_add(kTtcHeaderPrefixSize, offsets_size, &collection_header_end)) {
            return fail(SfntParseErrorKind::ArithmeticOverflow, 8U, 0U,
                        "TTC offset array size overflow", error);
        }
        if (version == kTtcVersion2 &&
            !checked_add(collection_header_end, kTtcVersion2SuffixSize,
                         &collection_header_end)) {
            return fail(SfntParseErrorKind::ArithmeticOverflow, 8U, 0U,
                        "TTC version 2 header size overflow", error);
        }
        if (collection_header_end > bytes.size()) {
            return fail(SfntParseErrorKind::TruncatedInput, bytes.size(), 0U,
                        "TTC face offset array or DSIG fields are truncated", error);
        }
        if (face_index >= face_count) {
            return fail(SfntParseErrorKind::InvalidFaceIndex, 8U, face_index,
                        "selected TTC face index is out of range", error);
        }
        for (std::uint32_t index = 0U; index < face_count; ++index) {
            std::uint32_t offset = 0U;
            const std::size_t offset_location =
                kTtcHeaderPrefixSize + static_cast<std::size_t>(index) * 4U;
            if (!read_u32(bytes, offset_location, &offset) ||
                offset < collection_header_end ||
                static_cast<std::size_t>(offset) > bytes.size() ||
                bytes.size() - static_cast<std::size_t>(offset) < kSfntDirectoryHeaderSize) {
                return fail(SfntParseErrorKind::InvalidCollection, offset_location, index,
                            "TTC face directory offset is invalid", error);
            }
            if (index == face_index) {
                directory_offset = offset;
            }
        }
        if (version == kTtcVersion2) {
            const std::size_t dsig_location =
                kTtcHeaderPrefixSize + static_cast<std::size_t>(face_count) * 4U;
            std::uint32_t dsig_tag = 0U;
            std::uint32_t dsig_length = 0U;
            std::uint32_t dsig_offset = 0U;
            if (!read_u32(bytes, dsig_location, &dsig_tag) ||
                !read_u32(bytes, dsig_location + 4U, &dsig_length) ||
                !read_u32(bytes, dsig_location + 8U, &dsig_offset)) {
                return fail(SfntParseErrorKind::TruncatedInput, dsig_location, 0U,
                            "TTC DSIG fields are truncated", error);
            }
            if (dsig_tag != 0U && dsig_tag != kDsigTag) {
                return fail(SfntParseErrorKind::InvalidCollection, dsig_location, 0U,
                            "TTC DSIG tag is invalid", error);
            }
            const std::uint64_t dsig_end =
                static_cast<std::uint64_t>(dsig_offset) + dsig_length;
            if ((dsig_length == 0U && dsig_offset != 0U) ||
                (dsig_length != 0U &&
                 (dsig_tag != kDsigTag || dsig_end > bytes.size()))) {
                return fail(SfntParseErrorKind::InvalidCollection, dsig_location + 4U, 0U,
                            "TTC DSIG range is invalid", error);
            }
        }
    } else if (face_index != 0U) {
        return fail(SfntParseErrorKind::InvalidFaceIndex, 0U, face_index,
                    "single-font resources only expose face index zero", error);
    }

    std::uint32_t sfnt_version = 0U;
    std::uint16_t table_count = 0U;
    std::uint16_t search_range = 0U;
    std::uint16_t entry_selector = 0U;
    std::uint16_t range_shift = 0U;
    if (!read_u32(bytes, directory_offset, &sfnt_version) ||
        !read_u16(bytes, static_cast<std::size_t>(directory_offset) + 4U, &table_count) ||
        !read_u16(bytes, static_cast<std::size_t>(directory_offset) + 6U, &search_range) ||
        !read_u16(bytes, static_cast<std::size_t>(directory_offset) + 8U, &entry_selector) ||
        !read_u16(bytes, static_cast<std::size_t>(directory_offset) + 10U, &range_shift)) {
        return fail(SfntParseErrorKind::TruncatedInput, directory_offset, 0U,
                    "sfnt table directory header is truncated", error);
    }

    SfntFlavor flavor = SfntFlavor::TrueType;
    if (sfnt_version == kTrueTypeVersion) {
        flavor = SfntFlavor::TrueType;
    } else if (sfnt_version == kOttoTag) {
        flavor = SfntFlavor::Cff;
    } else {
        return fail(SfntParseErrorKind::UnsupportedSfntVersion, directory_offset, 0U,
                    "unsupported sfnt version", error);
    }
    if (table_count == 0U || table_count > kMaxTablesPerFace) {
        return fail(SfntParseErrorKind::InvalidTableCount,
                    static_cast<std::size_t>(directory_offset) + 4U,
                    table_count,
                    "sfnt table count is zero or exceeds the parser limit", error);
    }

    std::size_t records_size = 0U;
    std::size_t directory_end = 0U;
    if (!checked_mul(static_cast<std::size_t>(table_count), kTableRecordSize,
                     &records_size) ||
        !checked_add(static_cast<std::size_t>(directory_offset) +
                         kSfntDirectoryHeaderSize,
                     records_size,
                     &directory_end)) {
        return fail(SfntParseErrorKind::ArithmeticOverflow, directory_offset, 0U,
                    "sfnt directory size overflow", error);
    }
    if (directory_end > bytes.size()) {
        return fail(SfntParseErrorKind::TruncatedInput, bytes.size(), 0U,
                    "sfnt table directory is truncated", error);
    }

    std::uint16_t expected_range = 0U;
    std::uint16_t expected_selector = 0U;
    std::uint16_t expected_shift = 0U;
    expected_search_parameters(table_count, &expected_range, &expected_selector,
                               &expected_shift);

    std::uint32_t previous_tag = 0U;
    std::uint64_t payload_bytes = 0U;
    std::uint32_t zero_length_tables = 0U;
    std::uint32_t unaligned_tables = 0U;
    for (std::uint16_t index = 0U; index < table_count; ++index) {
        SfntTableRecord record;
        if (!read_record(bytes, directory_offset, index, &record)) {
            return fail(SfntParseErrorKind::TruncatedInput, directory_end, index,
                        "sfnt table record is truncated", error);
        }
        const std::size_t record_offset =
            static_cast<std::size_t>(directory_offset) + kSfntDirectoryHeaderSize +
            static_cast<std::size_t>(index) * kTableRecordSize;
        if (!valid_tag(record.tag)) {
            return fail(SfntParseErrorKind::InvalidTableTag, record_offset, index,
                        "sfnt table tag contains non-printable bytes", error);
        }
        if (index != 0U && record.tag <= previous_tag) {
            const SfntParseErrorKind kind = record.tag == previous_tag
                                                ? SfntParseErrorKind::DuplicateTableTag
                                                : SfntParseErrorKind::UnsortedTableDirectory;
            return fail(kind, record_offset, index,
                        record.tag == previous_tag
                            ? "sfnt table directory contains a duplicate tag"
                            : "sfnt table directory is not sorted by tag",
                        error);
        }
        previous_tag = record.tag;

        const std::uint64_t table_end =
            static_cast<std::uint64_t>(record.offset) + record.length;
        if (table_end > bytes.size()) {
            return fail(SfntParseErrorKind::InvalidTableRange, record_offset + 8U,
                        index, "sfnt table range escapes the resource", error);
        }
        if (record.length == 0U) {
            ++zero_length_tables;
        } else {
            if (intervals_overlap(record.offset, record.length, directory_offset,
                                  directory_end - directory_offset) ||
                (container_kind == SfntContainerKind::Collection &&
                 intervals_overlap(record.offset, record.length, 0U,
                                   collection_header_end))) {
                return fail(SfntParseErrorKind::DirectoryOverlap, record_offset + 8U,
                            index, "sfnt table overlaps a container or face directory",
                            error);
            }
            if ((record.offset & 3U) != 0U) {
                ++unaligned_tables;
            }
        }
        if (record.length >
            std::numeric_limits<std::uint64_t>::max() - payload_bytes) {
            return fail(SfntParseErrorKind::ArithmeticOverflow, record_offset + 12U,
                        index, "sfnt payload byte count overflow", error);
        }
        payload_bytes += record.length;

        for (std::uint16_t previous = 0U; previous < index; ++previous) {
            SfntTableRecord earlier;
            if (!read_record(bytes, directory_offset, previous, &earlier)) {
                return fail(SfntParseErrorKind::TruncatedInput, record_offset, index,
                            "earlier sfnt table record became unreadable", error);
            }
            if (intervals_overlap(record.offset, record.length, earlier.offset,
                                  earlier.length)) {
                return fail(SfntParseErrorKind::TableOverlap, record_offset + 8U,
                            index, "sfnt table ranges overlap", error);
            }
        }
    }

    output->bytes_ = bytes;
    output->directory_offset_ = directory_offset;
    output->face_count_ = face_count;
    output->face_index_ = face_index;
    output->table_count_ = table_count;
    output->container_kind_ = container_kind;
    output->flavor_ = flavor;

    if (stats != nullptr) {
        stats->face_count = face_count;
        stats->selected_face_index = face_index;
        stats->selected_face_offset = directory_offset;
        stats->table_count = table_count;
        stats->table_payload_bytes = payload_bytes;
        stats->zero_length_tables = zero_length_tables;
        stats->unaligned_tables = unaligned_tables;
        stats->search_parameters_match =
            search_range == expected_range &&
            entry_selector == expected_selector &&
            range_shift == expected_shift;
    }
    return true;
}

} // namespace zevryon::text
