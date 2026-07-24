#include "opentype_directory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using zevryon::text::OpenTypeDirectory;
using zevryon::text::OpenTypeDirectoryError;
using zevryon::text::OpenTypeDirectoryErrorKind;
using zevryon::text::OpenTypeDirectoryOptions;
using zevryon::text::OpenTypeDirectoryStats;
using zevryon::text::OpenTypeTag;
using zevryon::text::open_type_tag;

struct TableSeed {
    OpenTypeTag tag{0};
    std::vector<std::byte> bytes;
};

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

void write_u16(std::vector<std::byte>* output, std::size_t offset, std::uint16_t value) {
    (*output)[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
    (*output)[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

void write_u32(std::vector<std::byte>* output, std::size_t offset, std::uint32_t value) {
    (*output)[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
    (*output)[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    (*output)[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    (*output)[offset + 3U] = static_cast<std::byte>(value & 0xffU);
}

std::uint32_t independent_checksum(
    std::span<const std::byte> table,
    bool zero_head_adjustment) {
    std::uint32_t sum = 0U;
    for (std::size_t offset = 0U; offset < table.size(); offset += 4U) {
        std::uint32_t word = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            const std::size_t position = offset + index;
            std::uint8_t value = 0U;
            if (position < table.size() &&
                !(zero_head_adjustment && position >= 8U && position < 12U)) {
                value = std::to_integer<std::uint8_t>(table[position]);
            }
            word = static_cast<std::uint32_t>((word << 8U) | value);
        }
        sum += word;
    }
    return sum;
}

std::size_t aligned_four(std::size_t value) {
    return (value + 3U) & ~std::size_t{3U};
}

std::vector<std::byte> make_sfnt(
    OpenTypeTag version,
    std::vector<TableSeed> tables) {
    std::sort(
        tables.begin(),
        tables.end(),
        [](const TableSeed& left, const TableSeed& right) {
            return left.tag < right.tag;
        });
    const std::size_t directory_bytes = 12U + tables.size() * 16U;
    std::size_t total = directory_bytes;
    for (const TableSeed& table : tables) {
        total = aligned_four(total);
        total += aligned_four(table.bytes.size());
    }
    std::vector<std::byte> output(total, std::byte{0});
    write_u32(&output, 0U, version);
    write_u16(&output, 4U, static_cast<std::uint16_t>(tables.size()));

    std::uint16_t selector = 0U;
    std::uint32_t power = 1U;
    while (power <= tables.size() / 2U) {
        power *= 2U;
        ++selector;
    }
    const std::uint16_t search_range = static_cast<std::uint16_t>(power * 16U);
    write_u16(&output, 6U, search_range);
    write_u16(&output, 8U, selector);
    write_u16(
        &output,
        10U,
        static_cast<std::uint16_t>(tables.size() * 16U - search_range));

    std::size_t table_offset = directory_bytes;
    for (std::size_t index = 0U; index < tables.size(); ++index) {
        table_offset = aligned_four(table_offset);
        const TableSeed& table = tables[index];
        const std::size_t record = 12U + index * 16U;
        write_u32(&output, record, table.tag);
        write_u32(
            &output,
            record + 4U,
            independent_checksum(
                table.bytes,
                table.tag == zevryon::text::kOpenTypeHeadTag));
        write_u32(&output, record + 8U, static_cast<std::uint32_t>(table_offset));
        write_u32(
            &output,
            record + 12U,
            static_cast<std::uint32_t>(table.bytes.size()));
        std::copy(
            table.bytes.begin(),
            table.bytes.end(),
            output.begin() + static_cast<std::ptrdiff_t>(table_offset));
        table_offset += aligned_four(table.bytes.size());
    }
    return output;
}

std::vector<TableSeed> standard_tables() {
    std::vector<std::byte> head(12U, std::byte{0});
    head[0] = std::byte{0x00};
    head[1] = std::byte{0x01};
    head[2] = std::byte{0x00};
    head[3] = std::byte{0x00};
    head[8] = std::byte{0xde};
    head[9] = std::byte{0xad};
    head[10] = std::byte{0xbe};
    head[11] = std::byte{0xef};
    return {
        {zevryon::text::kOpenTypeOs2Tag,
         {std::byte{0x00}, std::byte{0x05}, std::byte{0x00}, std::byte{0x01}}},
        {zevryon::text::kOpenTypeCmapTag,
         {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x7f}}},
        {zevryon::text::kOpenTypeHeadTag, std::move(head)},
        {zevryon::text::kOpenTypeMaxpTag,
         {std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}}},
        {zevryon::text::kOpenTypeNameTag,
         {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}}},
    };
}

std::vector<std::byte> make_ttc_two_faces() {
    constexpr std::size_t first_directory = 20U;
    constexpr std::size_t second_directory = 48U;
    constexpr std::size_t shared_table = 76U;
    std::vector<std::byte> output(88U, std::byte{0});
    write_u32(&output, 0U, zevryon::text::kOpenTypeCollectionTag);
    write_u32(&output, 4U, 0x00010000U);
    write_u32(&output, 8U, 2U);
    write_u32(&output, 12U, first_directory);
    write_u32(&output, 16U, second_directory);

    std::vector<std::byte> head(12U, std::byte{0});
    head[0] = std::byte{0x00};
    head[1] = std::byte{0x01};
    const std::uint32_t checksum = independent_checksum(head, true);
    for (std::size_t directory : {first_directory, second_directory}) {
        write_u32(&output, directory, zevryon::text::kOpenTypeTrueTypeVersion);
        write_u16(&output, directory + 4U, 1U);
        write_u16(&output, directory + 6U, 16U);
        write_u16(&output, directory + 8U, 0U);
        write_u16(&output, directory + 10U, 0U);
        write_u32(&output, directory + 12U, zevryon::text::kOpenTypeHeadTag);
        write_u32(&output, directory + 16U, checksum);
        write_u32(&output, directory + 20U, shared_table);
        write_u32(&output, directory + 24U, static_cast<std::uint32_t>(head.size()));
    }
    std::copy(
        head.begin(),
        head.end(),
        output.begin() + static_cast<std::ptrdiff_t>(shared_table));
    return output;
}

bool parse(
    const std::vector<std::byte>& source,
    std::uint32_t face_index,
    const OpenTypeDirectoryOptions& options,
    std::size_t hard_limit,
    std::shared_ptr<const OpenTypeDirectory>* output,
    OpenTypeDirectoryStats* stats,
    OpenTypeDirectoryError* error) {
    return zevryon::text::parse_opentype_directory(
        source,
        face_index,
        options,
        hard_limit,
        output,
        stats,
        error);
}

bool test_valid_single_font() {
    const auto source = make_sfnt(
        zevryon::text::kOpenTypeTrueTypeVersion,
        standard_tables());
    std::shared_ptr<const OpenTypeDirectory> directory;
    OpenTypeDirectoryStats stats;
    OpenTypeDirectoryError error;
    if (!require(
            parse(source, 0U, {}, 4096U, &directory, &stats, &error),
            "valid SFNT failed: " + error.message) ||
        !require(directory != nullptr, "valid parse published no directory") ||
        !require(directory->tables().size() == 5U, "unexpected table count") ||
        !require(directory->resource_snapshot().current_bytes == 80U,
                 "persistent directory is not exactly 16 bytes per table") ||
        !require(directory->accounting_clean(), "directory accounting is not clean") ||
        !require(directory->within_hard_limit(), "directory exceeded hard limit") ||
        !require(stats.checksums_verified == 5U, "not all table checksums were verified") ||
        !require(stats.required_table_views == 5U, "required table view count is wrong")) {
        return false;
    }
    const auto cmap = zevryon::text::opentype_table_bytes(
        source,
        *directory,
        zevryon::text::kOpenTypeCmapTag);
    return require(cmap.size() == 5U, "zero-copy cmap view has wrong size") &&
        require(cmap[4] == std::byte{0x7f}, "zero-copy cmap view has wrong data") &&
        require(
            zevryon::text::find_opentype_table(
                *directory,
                open_type_tag('G', 'S', 'U', 'B')) == nullptr,
            "missing table lookup returned a record");
}

bool test_search_field_policy() {
    auto source = make_sfnt(
        zevryon::text::kOpenTypeTrueTypeVersion,
        standard_tables());
    write_u16(&source, 6U, 0U);
    std::shared_ptr<const OpenTypeDirectory> directory;
    OpenTypeDirectoryStats stats;
    OpenTypeDirectoryError error;
    OpenTypeDirectoryOptions permissive;
    if (!require(
            parse(source, 0U, permissive, 4096U, &directory, &stats, &error),
            "derived search-field mode rejected a font") ||
        !require(stats.search_field_mismatches == 1U, "search mismatch was not counted")) {
        return false;
    }
    OpenTypeDirectoryOptions strict;
    strict.reject_invalid_search_fields = true;
    return require(
               !parse(source, 0U, strict, 4096U, &directory, &stats, &error),
               "strict search-field policy accepted invalid fields") &&
        require(
            error.kind == OpenTypeDirectoryErrorKind::InvalidSearchFields,
            "strict search-field rejection has wrong error kind");
}

bool test_collection_face_selection() {
    const auto source = make_ttc_two_faces();
    std::shared_ptr<const OpenTypeDirectory> directory;
    OpenTypeDirectoryStats stats;
    OpenTypeDirectoryError error;
    if (!require(
            parse(source, 1U, {}, 4096U, &directory, &stats, &error),
            "valid TTC face failed: " + error.message) ||
        !require(directory->face_index() == 1U, "wrong TTC face selected") ||
        !require(directory->collection_face_count() == 2U, "wrong TTC face count") ||
        !require(directory->directory_offset() == 48U, "wrong TTC directory offset")) {
        return false;
    }
    return require(
               !parse(source, 2U, {}, 4096U, &directory, &stats, &error),
               "out-of-range TTC face succeeded") &&
        require(
            error.kind == OpenTypeDirectoryErrorKind::FaceIndexOutOfRange,
            "out-of-range TTC face has wrong error kind");
}

bool test_tag_and_range_failures() {
    const auto original = make_sfnt(
        zevryon::text::kOpenTypeTrueTypeVersion,
        standard_tables());
    std::shared_ptr<const OpenTypeDirectory> directory;
    OpenTypeDirectoryStats stats;
    OpenTypeDirectoryError error;
    OpenTypeDirectoryOptions no_checksum;
    no_checksum.verify_table_checksums = false;

    auto invalid_tag = original;
    write_u32(&invalid_tag, 12U, 0x01020304U);
    if (!require(
            !parse(invalid_tag, 0U, no_checksum, 4096U, &directory, &stats, &error),
            "non-printable table tag succeeded") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::InvalidTableTag,
                 "non-printable tag has wrong error")) {
        return false;
    }

    auto duplicate = original;
    write_u32(&duplicate, 28U, zevryon::text::kOpenTypeOs2Tag);
    if (!require(
            !parse(duplicate, 0U, no_checksum, 4096U, &directory, &stats, &error),
            "duplicate table tag succeeded") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::DuplicateOrUnsortedTag,
                 "duplicate tag has wrong error")) {
        return false;
    }

    auto misaligned = original;
    const std::uint32_t first_offset =
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(misaligned[20])) << 24U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(misaligned[21])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(misaligned[22])) << 8U) |
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(misaligned[23]));
    write_u32(&misaligned, 20U, first_offset + 1U);
    if (!require(
            !parse(misaligned, 0U, no_checksum, 4096U, &directory, &stats, &error),
            "misaligned table succeeded") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::MisalignedTable,
                 "misaligned table has wrong error")) {
        return false;
    }

    auto directory_overlap = original;
    write_u32(&directory_overlap, 20U, 12U);
    if (!require(
            !parse(directory_overlap, 0U, no_checksum, 4096U, &directory, &stats, &error),
            "table-directory overlap succeeded") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::TableOverlapsDirectory,
                 "directory overlap has wrong error")) {
        return false;
    }

    auto table_overlap = original;
    write_u32(&table_overlap, 36U, first_offset);
    return require(
               !parse(table_overlap, 0U, no_checksum, 4096U, &directory, &stats, &error),
               "overlapping tables succeeded") &&
        require(error.kind == OpenTypeDirectoryErrorKind::TableOverlap,
                "overlapping tables have wrong error");
}

bool test_padding_checksum_and_budget_failures() {
    const auto original = make_sfnt(
        zevryon::text::kOpenTypeTrueTypeVersion,
        standard_tables());
    std::shared_ptr<const OpenTypeDirectory> directory;
    OpenTypeDirectoryStats stats;
    OpenTypeDirectoryError error;

    auto bad_checksum = original;
    bad_checksum.back() ^= std::byte{0x01};
    if (!require(
            !parse(bad_checksum, 0U, {}, 4096U, &directory, &stats, &error),
            "checksum corruption succeeded") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::ChecksumMismatch,
                 "checksum corruption has wrong error")) {
        return false;
    }

    auto bad_padding = original;
    const auto valid = make_sfnt(
        zevryon::text::kOpenTypeTrueTypeVersion,
        standard_tables());
    std::shared_ptr<const OpenTypeDirectory> valid_directory;
    if (!parse(valid, 0U, {}, 4096U, &valid_directory, &stats, &error)) {
        return false;
    }
    const auto* cmap = zevryon::text::find_opentype_table(
        *valid_directory,
        zevryon::text::kOpenTypeCmapTag);
    if (cmap == nullptr) {
        return false;
    }
    bad_padding[static_cast<std::size_t>(cmap->offset) + cmap->length] = std::byte{0x44};
    if (!require(
            !parse(bad_padding, 0U, {}, 4096U, &directory, &stats, &error),
            "non-zero table padding succeeded") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::NonZeroPadding,
                 "padding corruption has wrong error")) {
        return false;
    }

    directory = valid_directory;
    if (!require(
            !parse(original, 0U, {}, 1U, &directory, &stats, &error),
            "one-byte directory budget succeeded") ||
        !require(directory == nullptr, "budget failure published a stale directory") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::OutputBudgetExceeded,
                 "budget failure has wrong error")) {
        return false;
    }

    std::vector<std::byte> head(12U, std::byte{0});
    head[0] = std::byte{0x12};
    head[8] = std::byte{0xff};
    head[9] = std::byte{0xee};
    head[10] = std::byte{0xdd};
    head[11] = std::byte{0xcc};
    return require(
        zevryon::text::calculate_opentype_table_checksum(head, true) == 0x12000000U,
        "head checksumAdjustment bytes were not treated as zero");
}

} // namespace

int main() {
    if (!test_valid_single_font() ||
        !test_search_field_policy() ||
        !test_collection_face_selection() ||
        !test_tag_and_range_failures() ||
        !test_padding_checksum_and_budget_failures()) {
        return 1;
    }
    return 0;
}
