#include "font_line_metrics.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace zevryon::text;

#define REQUIRE(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "requirement failed at line " << __LINE__ << ": "  \
                      << #condition << '\n';                                   \
            std::exit(EXIT_FAILURE);                                           \
        }                                                                      \
    } while (false)

void write_u16(
    std::vector<std::byte>* bytes,
    std::size_t offset,
    std::uint16_t value) {
    (*bytes)[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
    (*bytes)[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

void write_s16(
    std::vector<std::byte>* bytes,
    std::size_t offset,
    std::int16_t value) {
    write_u16(bytes, offset, static_cast<std::uint16_t>(value));
}

void write_u32(
    std::vector<std::byte>* bytes,
    std::size_t offset,
    std::uint32_t value) {
    (*bytes)[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
    (*bytes)[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    (*bytes)[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    (*bytes)[offset + 3U] = static_cast<std::byte>(value & 0xffU);
}

struct Table final {
    std::uint32_t tag{0};
    std::vector<std::byte> bytes;
};

std::vector<std::byte> make_sfnt(std::vector<Table> tables) {
    std::sort(
        tables.begin(),
        tables.end(),
        [](const Table& left, const Table& right) {
            return left.tag < right.tag;
        });
    const std::size_t directory_size = 12U + tables.size() * 16U;
    std::size_t total_size = directory_size;
    for (const Table& table : tables) {
        total_size = (total_size + 3U) & ~std::size_t{3U};
        total_size += table.bytes.size();
    }
    std::vector<std::byte> bytes(total_size);
    write_u32(&bytes, 0U, 0x00010000U);
    write_u16(&bytes, 4U, static_cast<std::uint16_t>(tables.size()));

    std::uint16_t power = 1U;
    std::uint16_t selector = 0U;
    while (static_cast<std::size_t>(power) * 2U <= tables.size()) {
        power = static_cast<std::uint16_t>(power * 2U);
        selector = static_cast<std::uint16_t>(selector + 1U);
    }
    const std::uint16_t search_range = static_cast<std::uint16_t>(power * 16U);
    write_u16(&bytes, 6U, search_range);
    write_u16(&bytes, 8U, selector);
    write_u16(
        &bytes,
        10U,
        static_cast<std::uint16_t>(tables.size() * 16U - search_range));

    std::size_t payload_offset = directory_size;
    for (std::size_t index = 0U; index < tables.size(); ++index) {
        payload_offset = (payload_offset + 3U) & ~std::size_t{3U};
        const std::size_t record_offset = 12U + index * 16U;
        write_u32(&bytes, record_offset, tables[index].tag);
        write_u32(&bytes, record_offset + 4U, 0U);
        write_u32(
            &bytes,
            record_offset + 8U,
            static_cast<std::uint32_t>(payload_offset));
        write_u32(
            &bytes,
            record_offset + 12U,
            static_cast<std::uint32_t>(tables[index].bytes.size()));
        std::copy(
            tables[index].bytes.begin(),
            tables[index].bytes.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset));
        payload_offset += tables[index].bytes.size();
    }
    return bytes;
}

Table make_head(std::uint16_t units_per_em) {
    Table table{sfnt_tag('h', 'e', 'a', 'd'), std::vector<std::byte>(54U)};
    write_u16(&table.bytes, 0U, 1U);
    write_u16(&table.bytes, 2U, 0U);
    write_u16(&table.bytes, 18U, units_per_em);
    return table;
}

Table make_hhea(
    std::int16_t ascender,
    std::int16_t descender,
    std::int16_t line_gap) {
    Table table{sfnt_tag('h', 'h', 'e', 'a'), std::vector<std::byte>(36U)};
    write_u32(&table.bytes, 0U, 0x00010000U);
    write_s16(&table.bytes, 4U, ascender);
    write_s16(&table.bytes, 6U, descender);
    write_s16(&table.bytes, 8U, line_gap);
    return table;
}

Table make_os2(
    bool use_typo,
    std::int16_t ascender,
    std::int16_t descender,
    std::int16_t line_gap,
    std::uint16_t win_ascent = 1000U,
    std::uint16_t win_descent = 300U) {
    Table table{sfnt_tag('O', 'S', '/', '2'), std::vector<std::byte>(78U)};
    write_u16(&table.bytes, 0U, 4U);
    write_u16(&table.bytes, 62U, use_typo ? 1U << 7U : 0U);
    write_s16(&table.bytes, 68U, ascender);
    write_s16(&table.bytes, 70U, descender);
    write_s16(&table.bytes, 72U, line_gap);
    write_u16(&table.bytes, 74U, win_ascent);
    write_u16(&table.bytes, 76U, win_descent);
    return table;
}

SfntResourceView open_view(const std::vector<std::byte>& bytes) {
    SfntResourceView view;
    SfntParseStats stats;
    SfntParseError error;
    REQUIRE(open_sfnt_resource(bytes, 0U, &view, &stats, &error));
    REQUIRE(view.valid());
    return view;
}

void test_use_typo_metrics_selects_os2() {
    const std::vector<std::byte> bytes = make_sfnt({
        make_head(1000U),
        make_hhea(700, -300, 0),
        make_os2(true, 800, -200, 100, 1100U, 350U)});
    const SfntResourceView view = open_view(bytes);
    FontLineMetricRecord record;
    FontLineMetricError error;
    REQUIRE(read_sfnt_font_line_metric_record(7U, view, &record, &error));
    REQUIRE(record.face_id == 7U);
    REQUIRE(record.units_per_em == 1000U);
    REQUIRE(record.ascender == 800);
    REQUIRE(record.descender == -200);
    REQUIRE(record.line_gap == 100);
    REQUIRE(record.win_ascent == 1100U);
    REQUIRE(record.win_descent == 350U);
    REQUIRE(record.source == FontLineMetricSource::Os2Typographic);
    REQUIRE((record.flags & kFontLineMetricUseTypoMetrics) != 0U);
}

void test_hhea_selected_without_use_typo_metrics() {
    const std::vector<std::byte> bytes = make_sfnt({
        make_head(2048U),
        make_hhea(1500, -500, 48),
        make_os2(false, 1600, -448, 0)});
    const SfntResourceView view = open_view(bytes);
    FontLineMetricRecord record;
    FontLineMetricError error;
    REQUIRE(read_sfnt_font_line_metric_record(9U, view, &record, &error));
    REQUIRE(record.source == FontLineMetricSource::HorizontalHeader);
    REQUIRE(record.ascender == 1500);
    REQUIRE(record.descender == -500);
    REQUIRE(record.line_gap == 48);
    REQUIRE((record.flags & kFontLineMetricHasHhea) != 0U);
    REQUIRE((record.flags & kFontLineMetricHasOs2) != 0U);
}

void test_os2_is_deterministic_fallback() {
    const std::vector<std::byte> bytes = make_sfnt({
        make_head(1000U),
        make_os2(false, 760, -240, 0)});
    const SfntResourceView view = open_view(bytes);
    FontLineMetricRecord record;
    FontLineMetricError error;
    REQUIRE(read_sfnt_font_line_metric_record(3U, view, &record, &error));
    REQUIRE(record.source == FontLineMetricSource::Os2TypographicFallback);
    REQUIRE(record.ascender == 760);
    REQUIRE(record.descender == -240);
}

void test_negative_gap_is_preserved() {
    const std::vector<std::byte> bytes = make_sfnt({
        make_head(1000U),
        make_hhea(800, -250, -50)});
    const SfntResourceView view = open_view(bytes);
    FontLineMetricRecord record;
    FontLineMetricError error;
    REQUIRE(read_sfnt_font_line_metric_record(4U, view, &record, &error));
    REQUIRE(record.line_gap == -50);
    REQUIRE((record.flags & kFontLineMetricNegativeLineGap) != 0U);
}

void test_invalid_upem_fails_closed() {
    const std::vector<std::byte> bytes = make_sfnt({
        make_head(15U),
        make_hhea(800, -200, 0)});
    const SfntResourceView view = open_view(bytes);
    FontLineMetricRecord record;
    record.face_id = 99U;
    FontLineMetricError error;
    REQUIRE(!read_sfnt_font_line_metric_record(5U, view, &record, &error));
    REQUIRE(error.kind == FontLineMetricErrorKind::InvalidHeadTable);
    REQUIRE(record.face_id == kInvalidFontFaceId);
}

void test_invalid_selected_typo_metrics_fail() {
    const std::vector<std::byte> bytes = make_sfnt({
        make_head(1000U),
        make_hhea(800, -200, 0),
        make_os2(true, -10, 20, 0)});
    const SfntResourceView view = open_view(bytes);
    FontLineMetricRecord record;
    FontLineMetricError error;
    REQUIRE(!read_sfnt_font_line_metric_record(6U, view, &record, &error));
    REQUIRE(error.kind == FontLineMetricErrorKind::InvalidMetrics);
}

void test_truncated_os2_fails_structurally() {
    Table os2 = make_os2(false, 800, -200, 0);
    os2.bytes.resize(64U);
    const std::vector<std::byte> bytes = make_sfnt({
        make_head(1000U),
        make_hhea(800, -200, 0),
        std::move(os2)});
    const SfntResourceView view = open_view(bytes);
    FontLineMetricRecord record;
    FontLineMetricError error;
    REQUIRE(!read_sfnt_font_line_metric_record(8U, view, &record, &error));
    REQUIRE(error.kind == FontLineMetricErrorKind::InvalidOs2Table);
}

} // namespace

int main() {
    test_use_typo_metrics_selects_os2();
    test_hhea_selected_without_use_typo_metrics();
    test_os2_is_deterministic_fallback();
    test_negative_gap_is_preserved();
    test_invalid_upem_fails_closed();
    test_invalid_selected_typo_metrics_fail();
    test_truncated_os2_fails_structurally();
    std::cout << "font line metric tests passed\n";
    return 0;
}
