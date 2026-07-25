#include "ledger_memory_resource.hpp"
#include "line_fragment_layout.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

using namespace zevryon::core;
using namespace zevryon::text;

constexpr std::uint32_t kClusterCount = 65'536U;
constexpr std::uint32_t kClustersPerLine = 64U;
constexpr std::uint32_t kClustersPerSegment = 16U;
constexpr std::uint32_t kLineCount = kClusterCount / kClustersPerLine;
constexpr std::uint32_t kSegmentsPerLine =
    kClustersPerLine / kClustersPerSegment;
constexpr std::uint32_t kSegmentCount =
    kLineCount * kSegmentsPerLine;
constexpr std::uint32_t kGlyphCount = kClusterCount / 2U;
constexpr std::uint32_t kFragmentCount = kSegmentCount;
constexpr std::uint64_t kGroupAdvance = 64U;
constexpr std::uint64_t kLineAdvance =
    (kClustersPerLine / 2U) * kGroupAdvance;
constexpr std::size_t kRetainedBytes =
    static_cast<std::size_t>(kLineCount) * sizeof(VisualLineLayoutRecord) +
    static_cast<std::size_t>(kFragmentCount) * sizeof(InlineLayoutFragment);
constexpr std::size_t kPeakBytes =
    static_cast<std::size_t>(kLineCount) * sizeof(BidiLineSpan) +
    static_cast<std::size_t>(kClusterCount) * sizeof(std::uint8_t) +
    static_cast<std::size_t>(kClusterCount) * sizeof(std::uint32_t) +
    static_cast<std::size_t>(kClusterCount) * 4U +
    (static_cast<std::size_t>(kClusterCount) + 1U) * sizeof(std::uint64_t) +
    kRetainedBytes;

static_assert(kLineCount == 1'024U);
static_assert(kSegmentCount == 4'096U);
static_assert(kGlyphCount == 32'768U);
static_assert(kFragmentCount == 4'096U);
static_assert(kRetainedBytes == 155'648U);
static_assert(kPeakBytes == 1'277'960U);

struct Fixture final {
    std::vector<GraphemeBoundary> boundaries;
    std::vector<BidiExplicitUnit> units;
    BidiSequenceTopology topology;
    std::vector<std::uint8_t> levels;
    MultiRunShapedText shaped;
    GlyphClusterMap cluster_map;
    LineSelection selection;
};

std::uint8_t level_for_segment(std::uint32_t segment_in_line) noexcept {
    constexpr std::array<std::uint8_t, 4U> levels{0U, 1U, 2U, 1U};
    return levels[segment_in_line];
}

Fixture make_fixture() {
    Fixture fixture;
    fixture.boundaries.reserve(static_cast<std::size_t>(kClusterCount) + 1U);
    fixture.units.reserve(kClusterCount);
    fixture.topology.active_unit_indices.reserve(kClusterCount);
    fixture.levels.reserve(kClusterCount);
    fixture.shaped.segments.reserve(kSegmentCount);
    fixture.cluster_map.records.resize(kClusterCount);
    fixture.selection.lines.reserve(kLineCount);

    for (std::uint32_t cluster = 0U; cluster <= kClusterCount; ++cluster) {
        fixture.boundaries.push_back(GraphemeBoundary{
            cluster,
            cluster});
    }

    for (std::uint32_t segment_index = 0U;
         segment_index < kSegmentCount;
         ++segment_index) {
        const std::uint32_t segment_in_line =
            segment_index % kSegmentsPerLine;
        const std::uint8_t level = level_for_segment(segment_in_line);
        const std::uint32_t first_cluster =
            segment_index * kClustersPerSegment;
        const std::uint32_t cluster_limit =
            first_cluster + kClustersPerSegment;

        fixture.shaped.segments.emplace_back(
            fixture.shaped.glyph_resource());
        MultiRunShapedSegment& segment = fixture.shaped.segments.back();
        segment.run = ShapingRunBoundary{
            first_cluster,
            static_cast<FontFaceId>(1U),
            ScriptId::Latn,
            (level & 1U) == 0U
                ? ShapingDirection::LeftToRight
                : ShapingDirection::RightToLeft,
            FontFallbackSource::Primary,
            level,
            0U};
        segment.glyphs.first_cluster = first_cluster;
        segment.glyphs.cluster_limit = cluster_limit;
        segment.glyphs.script = segment.run.script;
        segment.glyphs.direction = segment.run.direction;
        segment.glyphs.glyphs.reserve(kClustersPerSegment / 2U);

        for (std::uint32_t pair = 0U;
             pair < kClustersPerSegment / 2U;
             ++pair) {
            const std::uint32_t logical_pair =
                (level & 1U) == 0U
                    ? pair
                    : (kClustersPerSegment / 2U - 1U - pair);
            const std::uint32_t owner_cluster =
                first_cluster + logical_pair * 2U;
            segment.glyphs.glyphs.push_back(ShapedGlyph{
                100U + pair,
                owner_cluster,
                static_cast<std::int32_t>(kGroupAdvance),
                0,
                0,
                0,
                0U});
        }

        for (std::uint32_t pair = 0U;
             pair < kClustersPerSegment / 2U;
             ++pair) {
            const std::uint32_t owner_cluster = first_cluster + pair * 2U;
            const std::uint32_t first_glyph =
                (level & 1U) == 0U
                    ? pair
                    : (kClustersPerSegment / 2U - 1U - pair);
            const GlyphClusterRecord record{
                segment_index,
                owner_cluster,
                first_glyph,
                1U};
            fixture.cluster_map.records[owner_cluster] = record;
            fixture.cluster_map.records[owner_cluster + 1U] = record;
        }

        for (std::uint32_t cluster = first_cluster;
             cluster < cluster_limit;
             ++cluster) {
            fixture.units.push_back(BidiExplicitUnit{
                cluster,
                cluster,
                (level & 1U) == 0U ? BidiClass::L : BidiClass::R,
                (level & 1U) == 0U ? BidiClass::L : BidiClass::R,
                level,
                0U});
            fixture.topology.active_unit_indices.push_back(cluster);
            fixture.levels.push_back(level);
        }
    }

    for (std::uint32_t line = 0U; line < kLineCount; ++line) {
        std::uint32_t flags = (line + 1U) % 16U == 0U
            ? static_cast<std::uint32_t>(kSelectedLineMandatoryBreak)
            : static_cast<std::uint32_t>(kSelectedLineSoftBreak);
        if (line + 1U == kLineCount) {
            flags |= kSelectedLineTextEnd;
        }
        fixture.selection.lines.push_back(SelectedLineRecord{
            kLineAdvance,
            (line + 1U) * kClustersPerLine,
            flags});
    }
    return fixture;
}

std::uint64_t mix(std::uint64_t checksum, std::uint64_t value) noexcept {
    checksum ^= value;
    checksum *= 1'099'511'628'211ULL;
    return checksum;
}

std::uint64_t checksum_of(const LineFragmentLayout& output) noexcept {
    std::uint64_t checksum = 1'469'598'103'934'665'603ULL;
    for (const VisualLineLayoutRecord& line : output.lines) {
        checksum = mix(checksum, line.inline_advance);
        checksum = mix(checksum, line.first_fragment);
        checksum = mix(checksum, line.fragment_count);
        checksum = mix(checksum, line.cluster_limit);
        checksum = mix(checksum, line.flags);
    }
    for (const InlineLayoutFragment& fragment : output.fragments) {
        checksum = mix(checksum, fragment.inline_offset);
        checksum = mix(checksum, fragment.inline_advance);
        checksum = mix(checksum, fragment.segment_index);
        checksum = mix(checksum, fragment.first_cluster);
        checksum = mix(checksum, fragment.cluster_limit);
        checksum = mix(checksum, fragment.bidi_level);
        checksum = mix(checksum, fragment.flags);
    }
    return checksum;
}

double percentile(const std::vector<double>& values, double quantile) {
    const std::size_t index = static_cast<std::size_t>(
        quantile * static_cast<double>(values.size() - 1U));
    return values[index];
}

} // namespace

int main() {
    Fixture fixture = make_fixture();
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::LayoutFragment, kPeakBytes);
    LedgerMemoryResource resource(
        ledger,
        ResourceClass::LayoutFragment);
    LineFragmentLayout output(&resource);
    LineFragmentLayoutStats stats;
    LineFragmentLayoutError error;
    const LineFragmentLayoutRequest request{
        fixture.boundaries,
        fixture.units,
        &fixture.topology,
        fixture.levels,
        0U,
        &fixture.shaped,
        &fixture.cluster_map,
        &fixture.selection,
        kClusterCount};

    for (std::size_t warmup = 0U; warmup < 4U; ++warmup) {
        if (!build_line_fragment_layout(request, &output, &stats, &error)) {
            std::cerr << line_fragment_layout_error_kind_name(error.kind)
                      << ": " << error.message << '\n';
            return EXIT_FAILURE;
        }
    }

    std::vector<double> samples;
    samples.reserve(40U);
    std::uint64_t expected_checksum = 0U;
    for (std::size_t iteration = 0U; iteration < 40U; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        if (!build_line_fragment_layout(request, &output, &stats, &error)) {
            std::cerr << line_fragment_layout_error_kind_name(error.kind)
                      << ": " << error.message << '\n';
            return EXIT_FAILURE;
        }
        const auto stop = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(
            stop - start).count());
        const std::uint64_t checksum = checksum_of(output);
        if (iteration == 0U) {
            expected_checksum = checksum;
        } else if (checksum != expected_checksum) {
            std::cerr << "non-deterministic fragment checksum\n";
            return EXIT_FAILURE;
        }
    }
    std::sort(samples.begin(), samples.end());

    const ResourceSnapshot snapshot =
        ledger.snapshot(ResourceClass::LayoutFragment);
    std::cout << std::fixed << std::setprecision(6)
              << '{'
              << "\"schema\":\"zevryon.line-fragment-layout-benchmark.v1\"," 
              << "\"input_clusters\":" << kClusterCount << ','
              << "\"input_lines\":" << kLineCount << ','
              << "\"input_segments\":" << kSegmentCount << ','
              << "\"input_glyphs\":" << kGlyphCount << ','
              << "\"input_active_units\":" << kClusterCount << ','
              << "\"output_lines\":" << stats.output_lines << ','
              << "\"output_fragments\":" << stats.output_fragments << ','
              << "\"rtl_fragments\":" << stats.rtl_fragments << ','
              << "\"l1_adjusted_fragments\":" << stats.l1_adjusted_fragments << ','
              << "\"x9_only_fragments\":" << stats.x9_only_fragments << ','
              << "\"l2_reversal_spans\":" << stats.l2_reversal_spans << ','
              << "\"l2_reversed_fragments\":" << stats.l2_reversed_fragments << ','
              << "\"line_record_bytes\":" << sizeof(VisualLineLayoutRecord) << ','
              << "\"fragment_record_bytes\":" << sizeof(InlineLayoutFragment) << ','
              << "\"retained_bytes\":" << snapshot.current_bytes << ','
              << "\"peak_bytes\":" << snapshot.peak_bytes << ','
              << "\"hard_limit_bytes\":" << snapshot.hard_limit_bytes << ','
              << "\"total_inline_advance\":" << stats.total_inline_advance << ','
              << "\"maximum_line_advance\":" << stats.maximum_line_advance << ','
              << "\"maximum_fragments_per_line\":" << stats.maximum_fragments_per_line << ','
              << "\"maximum_fragment_level\":"
              << static_cast<unsigned int>(stats.maximum_fragment_level) << ','
              << "\"checksum\":" << expected_checksum << ','
              << "\"p50_ms\":" << percentile(samples, 0.50) << ','
              << "\"p95_ms\":" << percentile(samples, 0.95) << ','
              << "\"p99_ms\":" << percentile(samples, 0.99) << ','
              << "\"maximum_ms\":" << samples.back() << ','
              << "\"within_hard_limits\":"
              << (ledger.within_hard_limits() ? "true" : "false") << ','
              << "\"accounting_clean\":"
              << (ledger.accounting_clean() ? "true" : "false")
              << "}\n";
    return EXIT_SUCCESS;
}
