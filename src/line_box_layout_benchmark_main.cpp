#include "ledger_memory_resource.hpp"
#include "line_box_layout.hpp"

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
constexpr std::uint32_t kFragmentsPerLine = 4U;
constexpr std::uint32_t kClustersPerFragment =
    kClustersPerLine / kFragmentsPerLine;
constexpr std::uint32_t kLineCount = kClusterCount / kClustersPerLine;
constexpr std::uint32_t kFragmentCount = kLineCount * kFragmentsPerLine;
constexpr std::uint64_t kInlineAdvancePerFragment = 512U;
constexpr std::uint64_t kInlineAdvancePerLine =
    kInlineAdvancePerFragment * kFragmentsPerLine;
constexpr std::size_t kRetainedBytes =
    static_cast<std::size_t>(kLineCount) * sizeof(LineBoxRecord) +
    static_cast<std::size_t>(kFragmentCount) * sizeof(FragmentBlockMetric);

static_assert(kLineCount == 1'024U);
static_assert(kFragmentCount == 4'096U);
static_assert(kRetainedBytes == 180'224U);

FontLineMetricRecord make_metric(
    FontFaceId face_id,
    std::int32_t ascender,
    std::int32_t descender,
    std::int32_t gap,
    FontLineMetricSource source) {
    std::uint8_t flags = source == FontLineMetricSource::HorizontalHeader
        ? static_cast<std::uint8_t>(kFontLineMetricHasHhea)
        : static_cast<std::uint8_t>(
              kFontLineMetricHasOs2 | kFontLineMetricUseTypoMetrics);
    if (gap < 0) {
        flags = static_cast<std::uint8_t>(
            flags | kFontLineMetricNegativeLineGap);
    }
    return FontLineMetricRecord{
        face_id,
        1000U,
        ascender,
        descender,
        gap,
        static_cast<std::uint32_t>(ascender),
        static_cast<std::uint32_t>(-descender),
        source,
        flags,
        0U};
}

struct Fixture final {
    FontLineMetricTable metrics;
    MultiRunShapedText shaped;
    LineFragmentLayout fragments;
};

Fixture make_fixture() {
    Fixture fixture;
    fixture.metrics.records.push_back(make_metric(
        1U, 800, -200, 0, FontLineMetricSource::Os2Typographic));
    fixture.metrics.records.push_back(make_metric(
        2U, 900, -300, 100, FontLineMetricSource::Os2Typographic));
    fixture.metrics.records.push_back(make_metric(
        3U, 700, -250, 50, FontLineMetricSource::HorizontalHeader));
    fixture.metrics.records.push_back(make_metric(
        4U, 850, -250, -100, FontLineMetricSource::Os2Typographic));

    fixture.shaped.segments.reserve(kFragmentCount);
    fixture.fragments.fragments.reserve(kFragmentCount);
    fixture.fragments.lines.reserve(kLineCount);

    constexpr std::array<std::int32_t, 4U> scales{1000, 1000, 1200, 1000};
    for (std::uint32_t fragment = 0U;
         fragment < kFragmentCount;
         ++fragment) {
        const std::uint32_t relative = fragment % kFragmentsPerLine;
        const std::uint32_t first_cluster =
            fragment * kClustersPerFragment;
        const std::uint32_t cluster_limit =
            first_cluster + kClustersPerFragment;
        const FontFaceId face_id = relative + 1U;
        fixture.shaped.segments.emplace_back(
            fixture.shaped.glyph_resource());
        MultiRunShapedSegment& segment = fixture.shaped.segments.back();
        segment.run = ShapingRunBoundary{
            first_cluster,
            face_id,
            ScriptId::Latn,
            (relative & 1U) == 0U
                ? ShapingDirection::LeftToRight
                : ShapingDirection::RightToLeft,
            FontFallbackSource::Primary,
            static_cast<std::uint8_t>(relative & 1U),
            0U};
        segment.glyphs.first_cluster = first_cluster;
        segment.glyphs.cluster_limit = cluster_limit;
        segment.glyphs.script = ScriptId::Latn;
        segment.glyphs.direction = segment.run.direction;
        segment.glyphs.x_scale = scales[relative];
        segment.glyphs.y_scale = scales[relative];

        fixture.fragments.fragments.push_back(InlineLayoutFragment{
            static_cast<std::uint64_t>(relative) * kInlineAdvancePerFragment,
            kInlineAdvancePerFragment,
            fragment,
            first_cluster,
            cluster_limit,
            static_cast<std::uint8_t>(relative & 1U),
            0U,
            0U});
    }

    for (std::uint32_t line = 0U; line < kLineCount; ++line) {
        fixture.fragments.lines.push_back(VisualLineLayoutRecord{
            kInlineAdvancePerLine,
            line * kFragmentsPerLine,
            kFragmentsPerLine,
            (line + 1U) * kClustersPerLine,
            0U});
    }
    return fixture;
}

std::uint64_t mix(std::uint64_t checksum, std::uint64_t value) noexcept {
    checksum ^= value;
    checksum *= 1'099'511'628'211ULL;
    return checksum;
}

std::uint64_t checksum_of(const LineBoxLayout& output) noexcept {
    std::uint64_t checksum = 1'469'598'103'934'665'603ULL;
    for (const LineBoxRecord& line : output.lines) {
        checksum = mix(checksum, line.block_start);
        checksum = mix(checksum, line.block_size);
        checksum = mix(checksum, line.baseline);
        checksum = mix(checksum, line.inline_advance);
        checksum = mix(checksum, line.first_fragment_metric);
        checksum = mix(checksum, line.fragment_metric_count);
        checksum = mix(checksum, line.cluster_limit);
        checksum = mix(checksum, line.flags);
    }
    for (const FragmentBlockMetric& metric : output.fragment_metrics) {
        checksum = mix(checksum, metric.block_offset);
        checksum = mix(checksum, metric.block_size);
        checksum = mix(checksum, metric.baseline_offset);
        checksum = mix(checksum, metric.flags);
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
    ledger.set_hard_limit(ResourceClass::LayoutFragment, kRetainedBytes);
    LedgerMemoryResource resource(ledger, ResourceClass::LayoutFragment);
    LineBoxLayout output(&resource);
    LineBoxLayoutStats stats;
    LineBoxLayoutError error;
    const LineBoxLayoutRequest request{
        &fixture.fragments,
        &fixture.shaped,
        &fixture.metrics,
        1U,
        1000};

    for (std::size_t warmup = 0U; warmup < 4U; ++warmup) {
        if (!build_line_box_layout(request, &output, &stats, &error)) {
            std::cerr << line_box_layout_error_kind_name(error.kind)
                      << ": " << error.message << '\n';
            return EXIT_FAILURE;
        }
    }

    std::vector<double> samples;
    samples.reserve(40U);
    std::uint64_t expected_checksum = 0U;
    for (std::size_t iteration = 0U; iteration < 40U; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        if (!build_line_box_layout(request, &output, &stats, &error)) {
            std::cerr << line_box_layout_error_kind_name(error.kind)
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
            std::cerr << "non-deterministic line-box checksum\n";
            return EXIT_FAILURE;
        }
    }
    std::sort(samples.begin(), samples.end());

    const ResourceSnapshot snapshot =
        ledger.snapshot(ResourceClass::LayoutFragment);
    std::cout << std::fixed << std::setprecision(6)
              << '{'
              << "\"schema\":\"zevryon.line-box-layout-benchmark.v1\","
              << "\"input_lines\":" << kLineCount << ','
              << "\"input_fragments\":" << kFragmentCount << ','
              << "\"input_segments\":" << kFragmentCount << ','
              << "\"input_metric_records\":4,"
              << "\"output_lines\":" << stats.output_lines << ','
              << "\"output_fragment_metrics\":"
              << stats.output_fragment_metrics << ','
              << "\"empty_lines\":" << stats.empty_lines << ','
              << "\"expanded_lines\":" << stats.expanded_lines << ','
              << "\"mixed_metric_lines\":" << stats.mixed_metric_lines << ','
              << "\"os2_fragment_metrics\":"
              << stats.os2_fragment_metrics << ','
              << "\"hhea_fragment_metrics\":"
              << stats.hhea_fragment_metrics << ','
              << "\"negative_gap_fragment_metrics\":"
              << stats.negative_gap_fragment_metrics << ','
              << "\"line_record_bytes\":" << sizeof(LineBoxRecord) << ','
              << "\"fragment_metric_bytes\":"
              << sizeof(FragmentBlockMetric) << ','
              << "\"retained_bytes\":" << snapshot.current_bytes << ','
              << "\"peak_bytes\":" << snapshot.peak_bytes << ','
              << "\"hard_limit_bytes\":" << snapshot.hard_limit_bytes << ','
              << "\"total_block_extent\":" << stats.total_block_extent << ','
              << "\"maximum_line_block_size\":"
              << stats.maximum_line_block_size << ','
              << "\"maximum_line_ascent\":"
              << stats.maximum_line_ascent << ','
              << "\"maximum_line_descent\":"
              << stats.maximum_line_descent << ','
              << "\"maximum_fragment_block_size\":"
              << stats.maximum_fragment_block_size << ','
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
