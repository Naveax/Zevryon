#include "line_selection.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

using namespace zevryon::core;
using namespace zevryon::text;

constexpr std::uint32_t kClusterCount = 65'536U;
constexpr std::size_t kSegmentCount = 2'048U;
constexpr std::uint32_t kClustersPerSegment = 32U;
constexpr std::uint32_t kGroupsPerSegment = 16U;
constexpr std::uint64_t kAvailableInlineAdvance = 2'048U;
constexpr std::size_t kExpectedLines = 1'024U;
constexpr std::size_t kExpectedCurrentBytes =
    kExpectedLines * sizeof(SelectedLineRecord);
constexpr std::size_t kExpectedPeakBytes =
    static_cast<std::size_t>(kClusterCount) * sizeof(std::uint64_t) +
    kExpectedCurrentBytes;
constexpr std::size_t kSamples = 64U;
constexpr std::size_t kBatch = 4U;

ShapedGlyph make_glyph(
    std::uint32_t glyph_id,
    std::uint32_t owner_cluster,
    bool rtl) {
    return ShapedGlyph{
        glyph_id,
        owner_cluster,
        rtl ? -64 : 64,
        0,
        0,
        0,
        0U};
}

bool build_fixture(MultiRunShapedText* text) {
    try {
        text->segments.reserve(kSegmentCount);
        for (std::size_t segment_index = 0U;
             segment_index < kSegmentCount;
             ++segment_index) {
            const std::uint32_t first = static_cast<std::uint32_t>(
                segment_index * kClustersPerSegment);
            const bool rtl = (segment_index & 1U) != 0U;
            text->segments.emplace_back(text->glyph_resource());
            MultiRunShapedSegment& segment = text->segments.back();
            segment.run = ShapingRunBoundary{
                first,
                static_cast<FontFaceId>(rtl ? 2U : 1U),
                rtl ? ScriptId::Arab : ScriptId::Latn,
                rtl ? ShapingDirection::RightToLeft
                    : ShapingDirection::LeftToRight,
                FontFallbackSource::Primary,
                static_cast<std::uint8_t>(rtl ? 1U : 0U),
                0U};
            segment.glyphs.first_cluster = first;
            segment.glyphs.cluster_limit = first + kClustersPerSegment;
            segment.glyphs.script = segment.run.script;
            segment.glyphs.direction = segment.run.direction;
            segment.glyphs.glyphs.reserve(kGroupsPerSegment);

            for (std::uint32_t physical_group = 0U;
                 physical_group < kGroupsPerSegment;
                 ++physical_group) {
                const std::uint32_t logical_group = rtl
                    ? kGroupsPerSegment - 1U - physical_group
                    : physical_group;
                const std::uint32_t owner =
                    first + logical_group * 2U;
                const std::uint32_t glyph_id = static_cast<std::uint32_t>(
                    100U + segment_index * kGroupsPerSegment +
                    logical_group);
                segment.glyphs.glyphs.push_back(
                    make_glyph(glyph_id, owner, rtl));
            }
        }
    } catch (...) {
        return false;
    }
    return text->segments.size() == kSegmentCount &&
           text->segments.back().glyphs.cluster_limit == kClusterCount;
}

void build_opportunities(LineBreakOpportunityMap* map) {
    map->opportunities.resize(
        static_cast<std::size_t>(kClusterCount) + 1U,
        static_cast<std::uint8_t>(LineBreakOpportunity::Prohibited));
    for (std::uint32_t boundary = 8U;
         boundary <= kClusterCount;
         boundary += 8U) {
        map->opportunities[static_cast<std::size_t>(boundary)] =
            static_cast<std::uint8_t>(
                boundary % 1'024U == 0U
                    ? LineBreakOpportunity::Mandatory
                    : LineBreakOpportunity::Allowed);
    }
}

double percentile(
    const std::vector<double>& sorted,
    double probability) {
    const double position = probability *
        static_cast<double>(sorted.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(
        lower + 1U,
        sorted.size() - 1U);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

std::uint64_t checksum(const LineSelection& selection) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const SelectedLineRecord& line : selection.lines) {
        value ^= line.inline_advance;
        value *= 1099511628211ULL;
        value ^= line.cluster_limit;
        value *= 1099511628211ULL;
        value ^= line.flags;
        value *= 1099511628211ULL;
    }
    return value;
}

} // namespace

int main() {
    MultiRunShapedText shaped_text;
    if (!build_fixture(&shaped_text)) {
        return 2;
    }

    GlyphClusterMap cluster_map;
    GlyphClusterMapStats cluster_stats;
    GlyphClusterMapError cluster_error;
    if (!build_glyph_cluster_map(
            shaped_text,
            kClusterCount,
            &cluster_map,
            &cluster_stats,
            &cluster_error)) {
        return 1;
    }

    CaretBoundaryMap caret_map;
    CaretBoundaryMapStats caret_stats;
    CaretBoundaryMapError caret_error;
    if (!build_caret_boundary_map(
            CaretBoundaryMapRequest{
                &shaped_text,
                &cluster_map,
                kClusterCount},
            &caret_map,
            &caret_stats,
            &caret_error)) {
        return 1;
    }

    LineBreakOpportunityMap opportunity_map;
    build_opportunities(&opportunity_map);

    ResourceLedger ledger;
    ledger.set_hard_limit(
        ResourceClass::LineSelectionMap,
        kExpectedPeakBytes);
    LedgerMemoryResource memory(
        ledger,
        ResourceClass::LineSelectionMap);
    LineSelection selection(&memory);
    LineSelectionStats stats;
    LineSelectionError error;

    auto run_once = [&]() {
        return select_bounded_lines(
            LineSelectionRequest{
                &shaped_text,
                &cluster_map,
                &caret_map,
                &opportunity_map,
                kClusterCount,
                kAvailableInlineAdvance},
            &selection,
            &stats,
            &error);
    };

    for (std::size_t warmup = 0U; warmup < 8U; ++warmup) {
        if (!run_once()) {
            return 1;
        }
    }

    std::vector<double> samples;
    samples.reserve(kSamples);
    for (std::size_t sample = 0U; sample < kSamples; ++sample) {
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t batch = 0U; batch < kBatch; ++batch) {
            if (!run_once()) {
                return 1;
            }
        }
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(end - start).count() /
            static_cast<double>(kBatch));
    }
    std::sort(samples.begin(), samples.end());

    const ResourceSnapshot snapshot =
        ledger.snapshot(ResourceClass::LineSelectionMap);
    if (cluster_map.records.size() != kClusterCount ||
        cluster_stats.output_records != kClusterCount ||
        caret_map.flags.size() !=
            static_cast<std::size_t>(kClusterCount) + 1U ||
        caret_stats.safe_boundaries != 32'769U ||
        caret_stats.merged_interior_boundaries != 32'768U ||
        stats.input_segments != kSegmentCount ||
        stats.input_glyphs != 32'768U ||
        stats.input_clusters != kClusterCount ||
        stats.input_boundaries != 65'537U ||
        stats.legal_boundaries != 8'192U ||
        stats.suppressed_unsafe_boundaries != 0U ||
        stats.zero_advance_clusters != 32'768U ||
        stats.output_lines != kExpectedLines ||
        stats.soft_break_lines != 960U ||
        stats.mandatory_break_lines != 64U ||
        stats.overflow_lines != 0U ||
        stats.empty_lines != 0U ||
        stats.total_inline_advance != 2'097'152U ||
        stats.maximum_line_advance != kAvailableInlineAdvance ||
        stats.maximum_overflow_advance != 0U ||
        stats.maximum_line_clusters != 64U ||
        selection.lines.size() != kExpectedLines ||
        selection.lines.back().cluster_limit != kClusterCount ||
        !selected_line_has_flag(
            selection,
            selection.lines.size() - 1U,
            kSelectedLineMandatoryBreak) ||
        !selected_line_has_flag(
            selection,
            selection.lines.size() - 1U,
            kSelectedLineTextEnd) ||
        snapshot.current_bytes != kExpectedCurrentBytes ||
        snapshot.peak_bytes != kExpectedPeakBytes ||
        snapshot.hard_limit_bytes != kExpectedPeakBytes ||
        snapshot.rejected_reservations != 0U ||
        snapshot.accounting_errors != 0U ||
        !ledger.within_hard_limits() ||
        !ledger.accounting_clean()) {
        return 1;
    }

    std::cout << std::fixed << std::setprecision(9)
              << "{\"schema\":\"zevryon.line-selection-benchmark.v1\","
              << "\"input_clusters\":" << kClusterCount << ','
              << "\"input_segments\":" << kSegmentCount << ','
              << "\"input_glyphs\":" << stats.input_glyphs << ','
              << "\"input_boundaries\":" << stats.input_boundaries << ','
              << "\"available_inline_advance\":"
              << kAvailableInlineAdvance << ','
              << "\"legal_boundaries\":" << stats.legal_boundaries << ','
              << "\"zero_advance_clusters\":"
              << stats.zero_advance_clusters << ','
              << "\"output_lines\":" << stats.output_lines << ','
              << "\"soft_break_lines\":" << stats.soft_break_lines << ','
              << "\"mandatory_break_lines\":"
              << stats.mandatory_break_lines << ','
              << "\"overflow_lines\":" << stats.overflow_lines << ','
              << "\"record_bytes\":" << sizeof(SelectedLineRecord) << ','
              << "\"current_bytes\":" << snapshot.current_bytes << ','
              << "\"peak_bytes\":" << snapshot.peak_bytes << ','
              << "\"hard_limit_bytes\":" << snapshot.hard_limit_bytes << ','
              << "\"total_inline_advance\":"
              << stats.total_inline_advance << ','
              << "\"maximum_line_advance\":"
              << stats.maximum_line_advance << ','
              << "\"maximum_line_clusters\":"
              << stats.maximum_line_clusters << ','
              << "\"checksum\":" << checksum(selection) << ','
              << "\"p50_ms\":" << percentile(samples, 0.50) << ','
              << "\"p95_ms\":" << percentile(samples, 0.95) << ','
              << "\"p99_ms\":" << percentile(samples, 0.99) << ','
              << "\"maximum_ms\":" << samples.back() << ','
              << "\"accounting_clean\":true,"
              << "\"within_hard_limits\":true}\n";
    return 0;
}
