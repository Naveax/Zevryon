#include "opentype_directory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace zevryon::text {
namespace {

constexpr std::size_t kSfntHeaderBytes = 12U;
constexpr std::size_t kTableRecordBytes = 16U;
constexpr std::size_t kTtcBaseHeaderBytes = 12U;
constexpr std::size_t kTtcV2DsigBytes = 12U;
constexpr OpenTypeTag kDsigTag = open_type_tag('D', 'S', 'I', 'G');

struct ProtectedRange {
    std::size_t first{0};
    std::size_t last{0};
};

void clear_error(OpenTypeDirectoryError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    OpenTypeDirectoryErrorKind kind,
    std::size_t face_index,
    std::size_t table_index,
    OpenTypeTag tag,
    const char* message,
    OpenTypeDirectoryError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->face_index = face_index;
        error->table_index = table_index;
        error->tag = tag;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool has_bytes(
    std::span<const std::byte> source,
    std::size_t offset,
    std::size_t length) noexcept {
    return offset <= source.size() && length <= source.size() - offset;
}

std::uint16_t read_u16(
    std::span<const std::byte> source,
    std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(source[offset])) << 8U) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(source[offset + 1U])));
}

std::uint32_t read_u32(
    std::span<const std::byte> source,
    std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[offset])) << 24U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[offset + 1U])) << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[offset + 2U])) << 8U) |
           static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(source[offset + 3U]));
}

bool printable_tag(OpenTypeTag tag) noexcept {
    for (unsigned shift : {24U, 16U, 8U, 0U}) {
        const std::uint32_t value = (tag >> shift) & 0xffU;
        if (value < 0x20U || value > 0x7eU) {
            return false;
        }
    }
    return true;
}

std::size_t align_four(std::size_t value) noexcept {
    return value > std::numeric_limits<std::size_t>::max() - 3U
        ? std::numeric_limits<std::size_t>::max()
        : (value + 3U) & ~std::size_t{3U};
}

bool ranges_overlap(
    std::size_t left_first,
    std::size_t left_last,
    std::size_t right_first,
    std::size_t right_last) noexcept {
    return left_first < right_last && right_first < left_last;
}

bool validate_directory_range(
    std::span<const std::byte> source,
    std::uint32_t offset,
    const OpenTypeDirectoryOptions& options,
    std::size_t face_index,
    ProtectedRange* output,
    OpenTypeDirectoryError* error) noexcept {
    const std::size_t directory_offset = static_cast<std::size_t>(offset);
    if ((directory_offset & 3U) != 0U ||
        !has_bytes(source, directory_offset, kSfntHeaderBytes)) {
        return fail(
            OpenTypeDirectoryErrorKind::DirectoryOutOfBounds,
            face_index,
            0U,
            0U,
            "font directory is misaligned or outside the source",
            error);
    }
    const OpenTypeTag version = read_u32(source, directory_offset);
    if (version != kOpenTypeTrueTypeVersion && version != kOpenTypeCffVersion) {
        return fail(
            OpenTypeDirectoryErrorKind::UnsupportedVersion,
            face_index,
            0U,
            version,
            "font directory has an unsupported sfntVersion",
            error);
    }
    const std::uint16_t table_count = read_u16(source, directory_offset + 4U);
    if (table_count == 0U || table_count > options.maximum_tables) {
        return fail(
            OpenTypeDirectoryErrorKind::InvalidTableCount,
            face_index,
            0U,
            0U,
            "font directory table count is zero or exceeds policy",
            error);
    }
    const std::size_t table_bytes =
        static_cast<std::size_t>(table_count) * kTableRecordBytes;
    if (!has_bytes(source, directory_offset + kSfntHeaderBytes, table_bytes)) {
        return fail(
            OpenTypeDirectoryErrorKind::DirectoryOutOfBounds,
            face_index,
            0U,
            0U,
            "font directory records exceed the source",
            error);
    }
    output->first = directory_offset;
    output->last = directory_offset + kSfntHeaderBytes + table_bytes;
    return true;
}

bool validate_ttc_header(
    std::span<const std::byte> source,
    std::uint32_t requested_face,
    const OpenTypeDirectoryOptions& options,
    std::pmr::vector<ProtectedRange>* protected_ranges,
    std::uint32_t* selected_directory,
    std::uint32_t* face_count,
    OpenTypeDirectoryError* error) {
    if (!has_bytes(source, 0U, kTtcBaseHeaderBytes)) {
        return fail(
            OpenTypeDirectoryErrorKind::InvalidCollection,
            requested_face,
            0U,
            0U,
            "TTC header is truncated",
            error);
    }
    const std::uint32_t version = read_u32(source, 4U);
    if (version != 0x00010000U && version != 0x00020000U) {
        return fail(
            OpenTypeDirectoryErrorKind::InvalidCollection,
            requested_face,
            0U,
            version,
            "TTC header version is not 1.0 or 2.0",
            error);
    }
    const std::uint32_t count = read_u32(source, 8U);
    if (count == 0U || count > options.maximum_collection_faces) {
        return fail(
            OpenTypeDirectoryErrorKind::InvalidCollection,
            requested_face,
            0U,
            0U,
            "TTC face count is zero or exceeds policy",
            error);
    }
    if (requested_face >= count) {
        return fail(
            OpenTypeDirectoryErrorKind::FaceIndexOutOfRange,
            requested_face,
            0U,
            0U,
            "requested TTC face index is out of range",
            error);
    }
    const std::size_t offsets_bytes = static_cast<std::size_t>(count) * 4U;
    const std::size_t header_bytes = kTtcBaseHeaderBytes + offsets_bytes +
        (version == 0x00020000U ? kTtcV2DsigBytes : 0U);
    if (!has_bytes(source, 0U, header_bytes)) {
        return fail(
            OpenTypeDirectoryErrorKind::InvalidCollection,
            requested_face,
            0U,
            0U,
            "TTC offset array or version-2 header is truncated",
            error);
    }

    protected_ranges->reserve(static_cast<std::size_t>(count) + 1U);
    protected_ranges->push_back(ProtectedRange{0U, header_bytes});
    std::uint32_t selected = 0U;
    for (std::uint32_t index = 0U; index < count; ++index) {
        const std::uint32_t directory = read_u32(
            source,
            kTtcBaseHeaderBytes + static_cast<std::size_t>(index) * 4U);
        ProtectedRange range;
        if (!validate_directory_range(
                source,
                directory,
                options,
                index,
                &range,
                error)) {
            return false;
        }
        protected_ranges->push_back(range);
        if (index == requested_face) {
            selected = directory;
        }
    }
    std::sort(
        protected_ranges->begin(),
        protected_ranges->end(),
        [](const ProtectedRange& left, const ProtectedRange& right) {
            return left.first < right.first ||
                (left.first == right.first && left.last < right.last);
        });
    for (std::size_t index = 1U; index < protected_ranges->size(); ++index) {
        if ((*protected_ranges)[index - 1U].last > (*protected_ranges)[index].first) {
            return fail(
                OpenTypeDirectoryErrorKind::InvalidCollection,
                requested_face,
                index,
                0U,
                "TTC header or font directories overlap",
                error);
        }
    }

    if (version == 0x00020000U) {
        const std::size_t dsig_fields = kTtcBaseHeaderBytes + offsets_bytes;
        const OpenTypeTag dsig_tag = read_u32(source, dsig_fields);
        const std::uint32_t dsig_length = read_u32(source, dsig_fields + 4U);
        const std::uint32_t dsig_offset = read_u32(source, dsig_fields + 8U);
        const bool all_zero = dsig_tag == 0U && dsig_length == 0U && dsig_offset == 0U;
        if (!all_zero) {
            const std::size_t offset = static_cast<std::size_t>(dsig_offset);
            const std::size_t length = static_cast<std::size_t>(dsig_length);
            if (dsig_tag != kDsigTag || length == 0U || (offset & 3U) != 0U ||
                !has_bytes(source, offset, length) || offset + length != source.size()) {
                return fail(
                    OpenTypeDirectoryErrorKind::InvalidCollection,
                    requested_face,
                    0U,
                    dsig_tag,
                    "TTC version-2 DSIG fields are inconsistent",
                    error);
            }
        }
    }

    *selected_directory = selected;
    *face_count = count;
    return true;
}

bool protected_overlap(
    const std::pmr::vector<ProtectedRange>& ranges,
    std::size_t first,
    std::size_t last) noexcept {
    const auto iterator = std::lower_bound(
        ranges.begin(),
        ranges.end(),
        first,
        [](const ProtectedRange& range, std::size_t value) {
            return range.last <= value;
        });
    return iterator != ranges.end() &&
        ranges_overlap(first, last, iterator->first, iterator->last);
}

std::uint32_t required_view_count(OpenTypeTag tag) noexcept {
    return tag == kOpenTypeCmapTag || tag == kOpenTypeHeadTag ||
           tag == kOpenTypeMaxpTag || tag == kOpenTypeNameTag ||
           tag == kOpenTypeOs2Tag || tag == kOpenTypeFvarTag ||
           tag == kOpenTypeColrTag || tag == kOpenTypeCpalTag
        ? 1U
        : 0U;
}

} // namespace

OpenTypeDirectory::OpenTypeDirectory(std::size_t hard_limit_bytes)
    : resource_(
          ledger_,
          core::ResourceClass::FontResourceDirectory),
      tables_(&resource_) {
    ledger_.set_hard_limit(
        core::ResourceClass::FontResourceDirectory,
        hard_limit_bytes);
}

OpenTypeContainerKind OpenTypeDirectory::container_kind() const noexcept {
    return container_kind_;
}

OpenTypeOutlineKind OpenTypeDirectory::outline_kind() const noexcept {
    return outline_kind_;
}

std::uint32_t OpenTypeDirectory::face_index() const noexcept {
    return face_index_;
}

std::uint32_t OpenTypeDirectory::collection_face_count() const noexcept {
    return collection_face_count_;
}

std::uint32_t OpenTypeDirectory::directory_offset() const noexcept {
    return directory_offset_;
}

std::uint32_t OpenTypeDirectory::sfnt_version() const noexcept {
    return sfnt_version_;
}

std::span<const OpenTypeTableRecord> OpenTypeDirectory::tables() const noexcept {
    return std::span<const OpenTypeTableRecord>(tables_.data(), tables_.size());
}

core::ResourceSnapshot OpenTypeDirectory::resource_snapshot() const noexcept {
    return ledger_.snapshot(core::ResourceClass::FontResourceDirectory);
}

bool OpenTypeDirectory::accounting_clean() const noexcept {
    return ledger_.accounting_clean();
}

bool OpenTypeDirectory::within_hard_limit() const noexcept {
    return ledger_.within_hard_limits();
}

const char* open_type_directory_error_kind_name(
    OpenTypeDirectoryErrorKind kind) noexcept {
    switch (kind) {
        case OpenTypeDirectoryErrorKind::None:
            return "none";
        case OpenTypeDirectoryErrorKind::InvalidInput:
            return "invalid_input";
        case OpenTypeDirectoryErrorKind::UnsupportedContainer:
            return "unsupported_container";
        case OpenTypeDirectoryErrorKind::UnsupportedVersion:
            return "unsupported_version";
        case OpenTypeDirectoryErrorKind::InvalidCollection:
            return "invalid_collection";
        case OpenTypeDirectoryErrorKind::FaceIndexOutOfRange:
            return "face_index_out_of_range";
        case OpenTypeDirectoryErrorKind::DirectoryOutOfBounds:
            return "directory_out_of_bounds";
        case OpenTypeDirectoryErrorKind::InvalidTableCount:
            return "invalid_table_count";
        case OpenTypeDirectoryErrorKind::InvalidSearchFields:
            return "invalid_search_fields";
        case OpenTypeDirectoryErrorKind::InvalidTableTag:
            return "invalid_table_tag";
        case OpenTypeDirectoryErrorKind::DuplicateOrUnsortedTag:
            return "duplicate_or_unsorted_tag";
        case OpenTypeDirectoryErrorKind::EmptyTable:
            return "empty_table";
        case OpenTypeDirectoryErrorKind::MisalignedTable:
            return "misaligned_table";
        case OpenTypeDirectoryErrorKind::TableOutOfBounds:
            return "table_out_of_bounds";
        case OpenTypeDirectoryErrorKind::TableOverlapsDirectory:
            return "table_overlaps_directory";
        case OpenTypeDirectoryErrorKind::TableOverlap:
            return "table_overlap";
        case OpenTypeDirectoryErrorKind::NonZeroPadding:
            return "non_zero_padding";
        case OpenTypeDirectoryErrorKind::ChecksumMismatch:
            return "checksum_mismatch";
        case OpenTypeDirectoryErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

bool parse_opentype_directory(
    std::span<const std::byte> source,
    std::uint32_t face_index,
    const OpenTypeDirectoryOptions& options,
    std::size_t hard_limit_bytes,
    std::shared_ptr<const OpenTypeDirectory>* output,
    OpenTypeDirectoryStats* stats,
    OpenTypeDirectoryError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->reset();
    *stats = {};
    clear_error(error);
    stats->source_bytes = static_cast<std::uint64_t>(source.size());

    if (source.size() < kSfntHeaderBytes ||
        source.size() > static_cast<std::size_t>(
                            std::numeric_limits<std::uint32_t>::max()) ||
        options.maximum_tables == 0U ||
        options.maximum_collection_faces == 0U) {
        return fail(
            OpenTypeDirectoryErrorKind::InvalidInput,
            face_index,
            0U,
            0U,
            "source size or parser policy is invalid",
            error);
    }

    try {
        auto candidate = std::make_shared<OpenTypeDirectory>(hard_limit_bytes);
        std::pmr::vector<ProtectedRange> protected_ranges(&candidate->resource_);
        std::uint32_t directory_offset = 0U;
        std::uint32_t collection_faces = 1U;
        const OpenTypeTag signature = read_u32(source, 0U);
        if (signature == kOpenTypeCollectionTag) {
            if (!validate_ttc_header(
                    source,
                    face_index,
                    options,
                    &protected_ranges,
                    &directory_offset,
                    &collection_faces,
                    error)) {
                return false;
            }
            candidate->container_kind_ = OpenTypeContainerKind::Collection;
        } else if (signature == kOpenTypeTrueTypeVersion ||
                   signature == kOpenTypeCffVersion) {
            if (face_index != 0U) {
                return fail(
                    OpenTypeDirectoryErrorKind::FaceIndexOutOfRange,
                    face_index,
                    0U,
                    0U,
                    "single-font resources only expose face zero",
                    error);
            }
            ProtectedRange directory;
            if (!validate_directory_range(
                    source,
                    0U,
                    options,
                    0U,
                    &directory,
                    error)) {
                return false;
            }
            protected_ranges.push_back(directory);
        } else {
            return fail(
                OpenTypeDirectoryErrorKind::UnsupportedContainer,
                face_index,
                0U,
                signature,
                "source is neither an OpenType font nor a font collection",
                error);
        }

        const std::size_t directory = static_cast<std::size_t>(directory_offset);
        const OpenTypeTag sfnt_version = read_u32(source, directory);
        const std::uint16_t table_count = read_u16(source, directory + 4U);
        const std::uint16_t stored_search_range = read_u16(source, directory + 6U);
        const std::uint16_t stored_entry_selector = read_u16(source, directory + 8U);
        const std::uint16_t stored_range_shift = read_u16(source, directory + 10U);

        std::uint32_t power = 1U;
        std::uint16_t selector = 0U;
        while (power <= static_cast<std::uint32_t>(table_count) / 2U) {
            power *= 2U;
            ++selector;
        }
        const std::uint32_t derived_search_range = power * 16U;
        const std::uint32_t derived_range_shift =
            static_cast<std::uint32_t>(table_count) * 16U - derived_search_range;
        const bool search_mismatch =
            stored_search_range != derived_search_range ||
            stored_entry_selector != selector ||
            stored_range_shift != derived_range_shift;
        if (search_mismatch) {
            ++stats->search_field_mismatches;
            if (options.reject_invalid_search_fields) {
                return fail(
                    OpenTypeDirectoryErrorKind::InvalidSearchFields,
                    face_index,
                    0U,
                    0U,
                    "stored SFNT binary-search fields are inconsistent",
                    error);
            }
        }

        candidate->tables_.reserve(table_count);
        OpenTypeTag previous_tag = 0U;
        for (std::size_t index = 0U; index < table_count; ++index) {
            const std::size_t record_offset =
                directory + kSfntHeaderBytes + index * kTableRecordBytes;
            const OpenTypeTableRecord record{
                read_u32(source, record_offset),
                read_u32(source, record_offset + 4U),
                read_u32(source, record_offset + 8U),
                read_u32(source, record_offset + 12U)};
            if (!printable_tag(record.tag)) {
                return fail(
                    OpenTypeDirectoryErrorKind::InvalidTableTag,
                    face_index,
                    index,
                    record.tag,
                    "table tag contains non-printable bytes",
                    error);
            }
            if (index != 0U && record.tag <= previous_tag) {
                return fail(
                    OpenTypeDirectoryErrorKind::DuplicateOrUnsortedTag,
                    face_index,
                    index,
                    record.tag,
                    "table tags are duplicated or not in ascending order",
                    error);
            }
            if (record.length == 0U) {
                return fail(
                    OpenTypeDirectoryErrorKind::EmptyTable,
                    face_index,
                    index,
                    record.tag,
                    "table has zero length",
                    error);
            }
            const std::size_t table_offset = static_cast<std::size_t>(record.offset);
            const std::size_t table_length = static_cast<std::size_t>(record.length);
            if ((table_offset & 3U) != 0U) {
                return fail(
                    OpenTypeDirectoryErrorKind::MisalignedTable,
                    face_index,
                    index,
                    record.tag,
                    "top-level table is not four-byte aligned",
                    error);
            }
            if (!has_bytes(source, table_offset, table_length)) {
                return fail(
                    OpenTypeDirectoryErrorKind::TableOutOfBounds,
                    face_index,
                    index,
                    record.tag,
                    "table bytes exceed the source",
                    error);
            }
            const std::size_t table_end = table_offset + table_length;
            if (protected_overlap(protected_ranges, table_offset, table_end)) {
                return fail(
                    OpenTypeDirectoryErrorKind::TableOverlapsDirectory,
                    face_index,
                    index,
                    record.tag,
                    "table overlaps the TTC header or a font directory",
                    error);
            }
            const std::size_t padded_end = align_four(table_end);
            if (padded_end == std::numeric_limits<std::size_t>::max() ||
                !has_bytes(source, table_end, padded_end - table_end)) {
                return fail(
                    OpenTypeDirectoryErrorKind::TableOutOfBounds,
                    face_index,
                    index,
                    record.tag,
                    "table padding exceeds the source",
                    error);
            }
            if (options.require_zero_padding) {
                for (std::size_t position = table_end;
                     position < padded_end;
                     ++position) {
                    if (source[position] != std::byte{0}) {
                        return fail(
                            OpenTypeDirectoryErrorKind::NonZeroPadding,
                            face_index,
                            index,
                            record.tag,
                            "table alignment padding is not zero",
                            error);
                    }
                }
            }
            if (options.verify_table_checksums) {
                const auto bytes = source.subspan(table_offset, table_length);
                const std::uint32_t computed = calculate_opentype_table_checksum(
                    bytes,
                    record.tag == kOpenTypeHeadTag);
                if (computed != record.checksum) {
                    return fail(
                        OpenTypeDirectoryErrorKind::ChecksumMismatch,
                        face_index,
                        index,
                        record.tag,
                        "table checksum does not match the directory",
                        error);
                }
                ++stats->checksums_verified;
            }
            candidate->tables_.push_back(record);
            previous_tag = record.tag;
            stats->table_bytes += record.length;
            stats->padding_bytes += padded_end - table_end;
            stats->required_table_views += required_view_count(record.tag);
        }

        std::pmr::vector<std::uint32_t> offset_order(&candidate->resource_);
        offset_order.reserve(table_count);
        for (std::uint32_t index = 0U; index < table_count; ++index) {
            offset_order.push_back(index);
        }
        std::sort(
            offset_order.begin(),
            offset_order.end(),
            [&candidate](std::uint32_t left, std::uint32_t right) {
                const OpenTypeTableRecord& left_record = candidate->tables_[left];
                const OpenTypeTableRecord& right_record = candidate->tables_[right];
                return left_record.offset < right_record.offset ||
                    (left_record.offset == right_record.offset &&
                     left_record.length < right_record.length);
            });
        for (std::size_t index = 1U; index < offset_order.size(); ++index) {
            const OpenTypeTableRecord& previous =
                candidate->tables_[offset_order[index - 1U]];
            const OpenTypeTableRecord& current =
                candidate->tables_[offset_order[index]];
            const std::uint64_t previous_end =
                static_cast<std::uint64_t>(previous.offset) + previous.length;
            if (previous_end > current.offset) {
                return fail(
                    OpenTypeDirectoryErrorKind::TableOverlap,
                    face_index,
                    offset_order[index],
                    current.tag,
                    "top-level table byte ranges overlap",
                    error);
            }
        }

        candidate->face_index_ = face_index;
        candidate->collection_face_count_ = collection_faces;
        candidate->directory_offset_ = directory_offset;
        candidate->sfnt_version_ = sfnt_version;
        candidate->outline_kind_ = sfnt_version == kOpenTypeCffVersion
            ? OpenTypeOutlineKind::Cff
            : OpenTypeOutlineKind::TrueType;

        stats->collection_faces = collection_faces;
        stats->tables = table_count;
        *output = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            OpenTypeDirectoryErrorKind::OutputBudgetExceeded,
            face_index,
            0U,
            0U,
            "OpenType directory allocation exceeded its hard budget",
            error);
    } catch (...) {
        return fail(
            OpenTypeDirectoryErrorKind::InvalidInput,
            face_index,
            0U,
            0U,
            "OpenType directory parsing failed",
            error);
    }
}

const OpenTypeTableRecord* find_opentype_table(
    const OpenTypeDirectory& directory,
    OpenTypeTag tag) noexcept {
    const auto tables = directory.tables();
    const auto iterator = std::lower_bound(
        tables.begin(),
        tables.end(),
        tag,
        [](const OpenTypeTableRecord& record, OpenTypeTag value) {
            return record.tag < value;
        });
    return iterator != tables.end() && iterator->tag == tag
        ? std::addressof(*iterator)
        : nullptr;
}

std::span<const std::byte> opentype_table_bytes(
    std::span<const std::byte> source,
    const OpenTypeDirectory& directory,
    OpenTypeTag tag) noexcept {
    const OpenTypeTableRecord* record = find_opentype_table(directory, tag);
    if (record == nullptr ||
        !has_bytes(source, record->offset, record->length)) {
        return {};
    }
    return source.subspan(record->offset, record->length);
}

std::uint32_t calculate_opentype_table_checksum(
    std::span<const std::byte> table,
    bool zero_head_checksum_adjustment) noexcept {
    std::uint32_t sum = 0U;
    for (std::size_t offset = 0U; offset < table.size(); offset += 4U) {
        std::uint32_t value = 0U;
        for (std::size_t byte_index = 0U; byte_index < 4U; ++byte_index) {
            const std::size_t position = offset + byte_index;
            std::uint8_t byte = 0U;
            if (position < table.size() &&
                !(zero_head_checksum_adjustment &&
                  position >= 8U && position < 12U)) {
                byte = std::to_integer<std::uint8_t>(table[position]);
            }
            value |= static_cast<std::uint32_t>(byte)
                << static_cast<unsigned>((3U - byte_index) * 8U);
        }
        sum += value;
    }
    return sum;
}

} // namespace zevryon::text
