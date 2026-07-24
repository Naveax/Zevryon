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

using namespace zevryon::text;

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

void put_u16(std::vector<std::byte>* bytes, std::size_t offset, std::uint16_t value) {
    (*bytes)[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
    (*bytes)[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

void put_u32(std::vector<std::byte>* bytes, std::size_t offset, std::uint32_t value) {
    (*bytes)[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
    (*bytes)[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    (*bytes)[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    (*bytes)[offset + 3U] = static_cast<std::byte>(value & 0xffU);
}

std::uint32_t get_u32(const std::vector<std::byte>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 8U) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U]));
}

std::uint32_t checksum(std::span<const std::byte> table, bool head) {
    std::uint32_t sum = 0U;
    for (std::size_t offset = 0U; offset < table.size(); offset += 4U) {
        std::uint32_t word = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            const std::size_t position = offset + index;
            std::uint8_t value = 0U;
            if (position < table.size() && !(head && position >= 8U && position < 12U)) {
                value = std::to_integer<std::uint8_t>(table[position]);
            }
            word = static_cast<std::uint32_t>((word << 8U) | value);
        }
        sum += word;
    }
    return sum;
}

std::size_t align4(std::size_t value) {
    return (value + 3U) & ~std::size_t{3U};
}

std::vector<TableSeed> standard_tables() {
    std::vector<std::byte> head(12U, std::byte{0});
    head[1] = std::byte{1};
    head[8] = std::byte{0xde};
    head[9] = std::byte{0xad};
    head[10] = std::byte{0xbe};
    head[11] = std::byte{0xef};
    return {
        {kOpenTypeCmapTag,
         {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x7f}}},
        {kOpenTypeHeadTag, std::move(head)},
        {kOpenTypeNameTag,
         {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}}},
    };
}

std::vector<std::byte> make_sfnt(std::vector<TableSeed> tables) {
    std::sort(tables.begin(), tables.end(), [](const auto& left, const auto& right) {
        return left.tag < right.tag;
    });
    const std::size_t directory_bytes = 12U + tables.size() * 16U;
    std::size_t total = directory_bytes;
    for (const auto& table : tables) {
        total = align4(total) + align4(table.bytes.size());
    }
    std::vector<std::byte> output(total, std::byte{0});
    put_u32(&output, 0U, kOpenTypeTrueTypeVersion);
    put_u16(&output, 4U, static_cast<std::uint16_t>(tables.size()));
    std::uint32_t power = 1U;
    std::uint16_t selector = 0U;
    while (power <= tables.size() / 2U) {
        power *= 2U;
        ++selector;
    }
    const auto search_range = static_cast<std::uint16_t>(power * 16U);
    put_u16(&output, 6U, search_range);
    put_u16(&output, 8U, selector);
    put_u16(
        &output,
        10U,
        static_cast<std::uint16_t>(tables.size() * 16U - search_range));

    std::size_t table_offset = directory_bytes;
    for (std::size_t index = 0U; index < tables.size(); ++index) {
        table_offset = align4(table_offset);
        const auto& table = tables[index];
        const std::size_t record = 12U + index * 16U;
        put_u32(&output, record, table.tag);
        put_u32(&output, record + 4U, checksum(table.bytes, table.tag == kOpenTypeHeadTag));
        put_u32(&output, record + 8U, static_cast<std::uint32_t>(table_offset));
        put_u32(&output, record + 12U, static_cast<std::uint32_t>(table.bytes.size()));
        std::copy(
            table.bytes.begin(),
            table.bytes.end(),
            output.begin() + static_cast<std::ptrdiff_t>(table_offset));
        table_offset += align4(table.bytes.size());
    }
    return output;
}

std::vector<std::byte> make_ttc() {
    constexpr std::size_t first_directory = 20U;
    constexpr std::size_t second_directory = 48U;
    constexpr std::size_t shared_head = 76U;
    std::vector<std::byte> output(88U, std::byte{0});
    put_u32(&output, 0U, kOpenTypeCollectionTag);
    put_u32(&output, 4U, 0x00010000U);
    put_u32(&output, 8U, 2U);
    put_u32(&output, 12U, first_directory);
    put_u32(&output, 16U, second_directory);
    std::vector<std::byte> head(12U, std::byte{0});
    head[1] = std::byte{1};
    const std::uint32_t head_checksum = checksum(head, true);
    for (std::size_t directory : {first_directory, second_directory}) {
        put_u32(&output, directory, kOpenTypeTrueTypeVersion);
        put_u16(&output, directory + 4U, 1U);
        put_u16(&output, directory + 6U, 16U);
        put_u32(&output, directory + 12U, kOpenTypeHeadTag);
        put_u32(&output, directory + 16U, head_checksum);
        put_u32(&output, directory + 20U, shared_head);
        put_u32(&output, directory + 24U, static_cast<std::uint32_t>(head.size()));
    }
    std::copy(
        head.begin(),
        head.end(),
        output.begin() + static_cast<std::ptrdiff_t>(shared_head));
    return output;
}

bool parse(
    const std::vector<std::byte>& source,
    std::uint32_t face,
    const OpenTypeDirectoryOptions& options,
    std::size_t budget,
    std::shared_ptr<const OpenTypeDirectory>* output,
    OpenTypeDirectoryStats* stats,
    OpenTypeDirectoryError* error) {
    return parse_opentype_directory(source, face, options, budget, output, stats, error);
}

bool valid_and_collection_tests() {
    const auto source = make_sfnt(standard_tables());
    std::shared_ptr<const OpenTypeDirectory> directory;
    OpenTypeDirectoryStats stats;
    OpenTypeDirectoryError error;
    if (!require(parse(source, 0U, {}, 4096U, &directory, &stats, &error),
                 "valid SFNT failed: " + error.message) ||
        !require(directory->tables().size() == 3U, "unexpected table count") ||
        !require(directory->resource_snapshot().current_bytes == 48U,
                 "persistent output is not 16 bytes per table") ||
        !require(directory->accounting_clean() && directory->within_hard_limit(),
                 "directory accounting failed") ||
        !require(stats.checksums_verified == 3U && stats.required_table_views == 3U,
                 "valid stats are incorrect")) {
        return false;
    }
    const auto cmap = opentype_table_bytes(source, *directory, kOpenTypeCmapTag);
    if (!require(cmap.size() == 5U && cmap[4] == std::byte{0x7f},
                 "zero-copy cmap view is incorrect") ||
        !require(find_opentype_table(*directory, open_type_tag('G', 'S', 'U', 'B')) == nullptr,
                 "missing table lookup succeeded")) {
        return false;
    }

    auto search_attack = source;
    put_u16(&search_attack, 6U, 0U);
    if (!require(parse(search_attack, 0U, {}, 4096U, &directory, &stats, &error),
                 "derived search fields rejected a font") ||
        !require(stats.search_field_mismatches == 1U, "search mismatch was not counted")) {
        return false;
    }
    OpenTypeDirectoryOptions strict_search;
    strict_search.reject_invalid_search_fields = true;
    if (!require(!parse(search_attack, 0U, strict_search, 4096U, &directory, &stats, &error),
                 "strict search policy accepted malformed fields") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::InvalidSearchFields,
                 "strict search failure has wrong error")) {
        return false;
    }

    const auto ttc = make_ttc();
    return require(parse(ttc, 1U, {}, 4096U, &directory, &stats, &error),
                   "valid TTC face failed: " + error.message) &&
        require(directory->face_index() == 1U && directory->collection_face_count() == 2U,
                "wrong TTC face metadata") &&
        require(!parse(ttc, 2U, {}, 4096U, &directory, &stats, &error),
                "out-of-range TTC face succeeded") &&
        require(error.kind == OpenTypeDirectoryErrorKind::FaceIndexOutOfRange,
                "out-of-range TTC error is wrong");
}

bool structural_failure_tests() {
    const auto source = make_sfnt(standard_tables());
    std::shared_ptr<const OpenTypeDirectory> directory;
    OpenTypeDirectoryStats stats;
    OpenTypeDirectoryError error;
    OpenTypeDirectoryOptions structural;
    structural.verify_table_checksums = false;
    structural.require_zero_padding = false;

    auto invalid_tag = source;
    put_u32(&invalid_tag, 12U, 0x01020304U);
    if (!require(!parse(invalid_tag, 0U, structural, 4096U, &directory, &stats, &error),
                 "invalid tag succeeded") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::InvalidTableTag,
                 "invalid tag error is wrong")) {
        return false;
    }

    auto duplicate = source;
    put_u32(&duplicate, 28U, kOpenTypeCmapTag);
    if (!require(!parse(duplicate, 0U, structural, 4096U, &directory, &stats, &error),
                 "duplicate tag succeeded") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::DuplicateOrUnsortedTag,
                 "duplicate tag error is wrong")) {
        return false;
    }

    const std::uint32_t first_offset = get_u32(source, 20U);
    auto misaligned = source;
    put_u32(&misaligned, 20U, first_offset + 1U);
    if (!require(!parse(misaligned, 0U, structural, 4096U, &directory, &stats, &error),
                 "misaligned table succeeded") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::MisalignedTable,
                 "misalignment error is wrong")) {
        return false;
    }

    auto directory_overlap = source;
    put_u32(&directory_overlap, 20U, 12U);
    if (!require(!parse(directory_overlap, 0U, structural, 4096U, &directory, &stats, &error),
                 "directory overlap succeeded") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::TableOverlapsDirectory,
                 "directory-overlap error is wrong")) {
        return false;
    }

    auto table_overlap = source;
    put_u32(&table_overlap, 36U, first_offset);
    return require(!parse(table_overlap, 0U, structural, 4096U, &directory, &stats, &error),
                   "table overlap succeeded") &&
        require(error.kind == OpenTypeDirectoryErrorKind::TableOverlap,
                "table-overlap error is wrong");
}

bool integrity_and_budget_tests() {
    const auto source = make_sfnt(standard_tables());
    std::shared_ptr<const OpenTypeDirectory> directory;
    OpenTypeDirectoryStats stats;
    OpenTypeDirectoryError error;
    if (!parse(source, 0U, {}, 4096U, &directory, &stats, &error)) {
        return false;
    }
    const auto pinned_valid = directory;

    auto checksum_attack = source;
    checksum_attack.back() ^= std::byte{1};
    if (!require(!parse(checksum_attack, 0U, {}, 4096U, &directory, &stats, &error),
                 "checksum corruption succeeded") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::ChecksumMismatch,
                 "checksum error is wrong")) {
        return false;
    }

    auto padding_attack = source;
    const auto* cmap = find_opentype_table(*pinned_valid, kOpenTypeCmapTag);
    if (cmap == nullptr) {
        return false;
    }
    padding_attack[static_cast<std::size_t>(cmap->offset) + cmap->length] = std::byte{0x44};
    if (!require(!parse(padding_attack, 0U, {}, 4096U, &directory, &stats, &error),
                 "non-zero padding succeeded") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::NonZeroPadding,
                 "padding error is wrong")) {
        return false;
    }

    if (!require(!parse(source, 0U, {}, 1U, &directory, &stats, &error),
                 "one-byte budget succeeded") ||
        !require(directory == nullptr, "budget failure published stale output") ||
        !require(pinned_valid != nullptr && pinned_valid->tables().size() == 3U,
                 "pinned valid directory did not survive later failures") ||
        !require(error.kind == OpenTypeDirectoryErrorKind::OutputBudgetExceeded,
                 "budget error is wrong")) {
        return false;
    }

    std::vector<std::byte> head(12U, std::byte{0});
    head[0] = std::byte{0x12};
    head[8] = std::byte{0xff};
    head[9] = std::byte{0xee};
    head[10] = std::byte{0xdd};
    head[11] = std::byte{0xcc};
    return require(
        calculate_opentype_table_checksum(head, true) == 0x12000000U,
        "head checksumAdjustment was not zeroed");
}

} // namespace

int main() {
    return valid_and_collection_tests() &&
            structural_failure_tests() &&
            integrity_and_budget_tests()
        ? 0
        : 1;
}
