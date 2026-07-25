#include "viewport_projection.hpp"

#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
using namespace zevryon::text;

constexpr std::uint32_t kDocumentLines = 16'384U;
constexpr std::uint32_t kFragmentsPerLine = 4U;
constexpr std::uint32_t kClusters =
    kDocumentLines * kFragmentsPerLine;
constexpr std::uint64_t kLineBlockSize = 1'000U;
constexpr std::uint64_t kFragmentAdvance = 256U;
constexpr std::uint64_t kLineAdvance = 1'024U;
constexpr std::uint32_t kProjectedLines = 80U;
constexpr std::uint32_t kProjectedFragments = 320U;
constexpr std::uint32_t kProjectedCarets = 640U;
constexpr std::uint32_t kProjectedSelectionRects = 64U;
constexpr std::size_t kRetainedBytes =
    kProjectedLines * sizeof(ViewportLineRecord) +
    kProjectedFragments * sizeof(ViewportFragmentRect) +
    kProjectedCarets * sizeof(ViewportCaretEdge) +
    kProjectedSelectionRects * sizeof(ViewportSelectionRect);
constexpr std::uint64_t kExpectedChecksum = 1'409'705'956'279'003'952ULL;
constexpr std::uint32_t kMeasuredIterations = 200U;
constexpr std::uint32_t kHitQueriesPerIteration = 256U;

void mix(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (unsigned int byte = 0U; byte < 8U; ++byte) {
        *hash ^= (value >> (byte * 8U)) & 0xffU;
        *hash *= 1'099'511'628'211ULL;
    }
}

double percentile(const std::vector<double>& values, double fraction) {
    const double rank = std::ceil(
        fraction * static_cast<double>(values.size()));
    const std::size_t index = static_cast<std::size_t>(
        std::max(1.0, rank) - 1.0);
    return values[std::min(index, values.size() - 1U)];
}

struct Fixture final {
    MultiRunShapedText shaped;
    GlyphClusterMap cluster_map;
    CaretBoundaryMap caret_map;
    LineFragmentLayout fragments;
    LineBoxLayout line_boxes;

    Fixture() {
        shaped.segments.emplace_back(std::pmr::get_default_resource());
        shaped.segments.back().glyphs.direction =
            ShapingDirection::LeftToRight;
        shaped.segments.back().glyphs.glyphs.push_back(
            {1U, 0U, 256, 0, 0, 0, 0U});

        shaped.segments.emplace_back(std::pmr::get_default_resource());
        shaped.segments.back().glyphs.direction =
            ShapingDirection::RightToLeft;
        shaped.segments.back().glyphs.glyphs.push_back(
            {2U, 0U, -256, 0, 0, 0, 0U});

        cluster_map.records.reserve(kClusters);
        caret_map.flags.resize(
            static_cast<std::size_t>(kClusters) + 1U,
            static_cast<std::uint8_t>(kCaretBoundarySafe));
        caret_map.flags.front() |=
            static_cast<std::uint8_t>(kCaretBoundaryTextEdge);
        caret_map.flags.back() |=
            static_cast<std::uint8_t>(kCaretBoundaryTextEdge);
        fragments.lines.reserve(kDocumentLines);
        fragments.fragments.reserve(kClusters);
        line_boxes.lines.reserve(kDocumentLines);
        line_boxes.fragment_metrics.reserve(kClusters);

        for (std::uint32_t line = 0U;
             line < kDocumentLines;
             ++line) {
            const std::uint32_t first_cluster = line * 4U;
            const std::uint32_t first_fragment =
                static_cast<std::uint32_t>(fragments.fragments.size());

            cluster_map.records.push_back(
                {0U, first_cluster, 0U, 1U});
            cluster_map.records.push_back(
                {0U, first_cluster + 1U, 0U, 1U});
            cluster_map.records.push_back(
                {1U, first_cluster + 2U, 0U, 1U});
            cluster_map.records.push_back(
                {1U, first_cluster + 3U, 0U, 1U});

            fragments.fragments.push_back({
                0U,
                kFragmentAdvance,
                0U,
                first_cluster,
                first_cluster + 1U,
                0U,
                0U,
                0U});
            fragments.fragments.push_back({
                kFragmentAdvance,
                kFragmentAdvance,
                1U,
                first_cluster + 3U,
                first_cluster + 4U,
                1U,
                static_cast<std::uint8_t>(
                    kInlineFragmentGlyphRunRtl),
                0U});
            fragments.fragments.push_back({
                kFragmentAdvance * 2U,
                kFragmentAdvance,
                1U,
                first_cluster + 2U,
                first_cluster + 3U,
                1U,
                static_cast<std::uint8_t>(
                    kInlineFragmentGlyphRunRtl),
                0U});
            fragments.fragments.push_back({
                kFragmentAdvance * 3U,
                kFragmentAdvance,
                0U,
                first_cluster + 1U,
                first_cluster + 2U,
                0U,
                0U,
                0U});
            fragments.lines.push_back({
                kLineAdvance,
                first_fragment,
                kFragmentsPerLine,
                first_cluster + 4U,
                kVisualLineContainsRtl});

            for (std::uint32_t fragment = 0U;
                 fragment < kFragmentsPerLine;
                 ++fragment) {
                line_boxes.fragment_metrics.push_back(
                    {0U, kLineBlockSize, 800U, 0U, 0U});
            }
            const std::uint64_t block_start =
                static_cast<std::uint64_t>(line) * kLineBlockSize;
            line_boxes.lines.push_back({
                block_start,
                kLineBlockSize,
                block_start + 800U,
                kLineAdvance,
                first_fragment,
                kFragmentsPerLine,
                first_cluster + 4U,
                0U});
        }
    }

    ViewportProjectionRequest request() const {
        ViewportProjectionRequest value;
        value.line_boxes = &line_boxes;
        value.fragment_layout = &fragments;
        value.shaped_text = &shaped;
        value.cluster_map = &cluster_map;
        value.caret_boundaries = &caret_map;
        value.viewport_inline_size = kLineAdvance;
        value.viewport_block_start = 8'192ULL * kLineBlockSize;
        value.viewport_block_size = 64ULL * kLineBlockSize;
        value.block_overscan = 8ULL * kLineBlockSize;
        value.selection = {
            (8'192U + 16U) * 4U,
            (8'192U + 32U) * 4U,
            true};
        value.limits = {
            kProjectedLines,
            kProjectedFragments,
            kProjectedCarets,
            kProjectedSelectionRects};
        return value;
    }
};

std::uint64_t checksum(const ViewportProjection& projection) {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    mix(&hash, projection.document_block_extent);
    for (const ViewportLineRecord& record : projection.lines) {
        mix(&hash, static_cast<std::uint64_t>(record.viewport_block_start));
        mix(&hash, static_cast<std::uint64_t>(record.viewport_baseline));
        mix(&hash, record.block_size);
        mix(&hash, record.inline_advance);
        mix(&hash, record.source_line_index);
        mix(&hash, record.first_fragment_rect);
        mix(&hash, record.fragment_rect_count);
        mix(&hash, record.first_caret);
        mix(&hash, record.caret_count);
        mix(&hash, record.first_selection_rect);
        mix(&hash, record.selection_rect_count);
        mix(&hash, record.flags);
    }
    for (const ViewportFragmentRect& record : projection.fragment_rects) {
        mix(&hash, static_cast<std::uint64_t>(record.viewport_inline_start));
        mix(&hash, static_cast<std::uint64_t>(record.viewport_block_start));
        mix(&hash, record.inline_size);
        mix(&hash, record.block_size);
        mix(&hash, record.source_fragment_index);
        mix(&hash, record.first_cluster);
        mix(&hash, record.cluster_limit);
        mix(&hash, record.flags);
    }
    for (const ViewportCaretEdge& record : projection.carets) {
        mix(&hash, static_cast<std::uint64_t>(record.viewport_inline_position));
        mix(&hash, static_cast<std::uint64_t>(record.viewport_block_start));
        mix(&hash, record.block_size);
        mix(&hash, record.boundary_index);
        mix(&hash, record.source_fragment_index);
        mix(&hash, record.flags);
    }
    for (const ViewportSelectionRect& record : projection.selection_rects) {
        mix(&hash, static_cast<std::uint64_t>(record.viewport_inline_start));
        mix(&hash, static_cast<std::uint64_t>(record.viewport_block_start));
        mix(&hash, record.inline_size);
        mix(&hash, record.block_size);
        mix(&hash, record.source_line_index);
        mix(&hash, record.source_fragment_index);
        mix(&hash, record.flags);
    }
    for (std::uint32_t query = 0U;
         query < kHitQueriesPerIteration;
         ++query) {
        ViewportHitTestResult hit;
        const std::int64_t inline_position =
            static_cast<std::int64_t>((query * 73U) % 1'280U) - 128;
        const std::int64_t block_position =
            static_cast<std::int64_t>((query * 997U) % 80'000U) - 8'000;
        if (!hit_test_viewport_projection(
                projection,
                inline_position,
                block_position,
                static_cast<ViewportHitTestBias>(query % 3U),
                &hit)) {
            return 0U;
        }
        mix(&hash, hit.source_line_index);
        mix(&hash, hit.source_fragment_index);
        mix(&hash, hit.boundary_index);
        mix(&hash, hit.flags);
        mix(&hash, hit.inline_distance);
        mix(&hash, hit.block_distance);
    }
    return hash;
}

} // namespace

int main() {
    using namespace zevryon;
    Fixture fixture;
    const ViewportProjectionRequest request = fixture.request();

    core::ResourceLedger ledger;
    ledger.set_hard_limit(
        core::ResourceClass::LayoutFragment,
        kRetainedBytes);
    core::LedgerMemoryResource resource(
        ledger,
        core::ResourceClass::LayoutFragment);
    ViewportProjection output(&resource);
    ViewportProjectionStats stats;
    ViewportProjectionError error;

    if (!build_viewport_projection(
            request,
            &output,
            &stats,
            &error)) {
        std::cerr << error.message << '\n';
        return 1;
    }

    std::vector<double> samples;
    samples.reserve(kMeasuredIterations);
    for (std::uint32_t iteration = 0U;
         iteration < kMeasuredIterations;
         ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        if (!build_viewport_projection(
                request,
                &output,
                &stats,
                &error)) {
            std::cerr << error.message << '\n';
            return 1;
        }
        for (std::uint32_t query = 0U;
             query < kHitQueriesPerIteration;
             ++query) {
            ViewportHitTestResult hit;
            const std::int64_t inline_position =
                static_cast<std::int64_t>((query * 73U) % 1'280U) - 128;
            const std::int64_t block_position =
                static_cast<std::int64_t>((query * 997U) % 80'000U) - 8'000;
            if (!hit_test_viewport_projection(
                    output,
                    inline_position,
                    block_position,
                    static_cast<ViewportHitTestBias>(query % 3U),
                    &hit)) {
                return 1;
            }
        }
        const auto stop = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(stop - start).count());
    }
    std::sort(samples.begin(), samples.end());

    const core::ResourceSnapshot snapshot = ledger.snapshot(
        core::ResourceClass::LayoutFragment);
    const std::uint64_t output_checksum = checksum(output);
    const bool within_hard_limits = ledger.within_hard_limits();
    const bool accounting_clean = ledger.accounting_clean();

    std::cout
        << "{\n"
        << "  \"schema\": \"zevryon.viewport-projection-benchmark.v1\",\n"
        << "  \"input_document_lines\": " << kDocumentLines << ",\n"
        << "  \"input_clusters\": " << kClusters << ",\n"
        << "  \"input_fragments\": " << kClusters << ",\n"
        << "  \"first_source_line\": " << stats.first_source_line << ",\n"
        << "  \"source_line_limit\": " << stats.source_line_limit << ",\n"
        << "  \"output_lines\": " << output.lines.size() << ",\n"
        << "  \"output_fragment_rects\": " << output.fragment_rects.size() << ",\n"
        << "  \"output_carets\": " << output.carets.size() << ",\n"
        << "  \"output_selection_rects\": " << output.selection_rects.size() << ",\n"
        << "  \"glyph_groups\": " << stats.glyph_groups << ",\n"
        << "  \"unsafe_caret_boundaries_skipped\": "
        << stats.unsafe_caret_boundaries_skipped << ",\n"
        << "  \"lines_before_viewport\": " << stats.lines_before_viewport << ",\n"
        << "  \"lines_after_viewport\": " << stats.lines_after_viewport << ",\n"
        << "  \"line_record_bytes\": " << sizeof(ViewportLineRecord) << ",\n"
        << "  \"fragment_record_bytes\": " << sizeof(ViewportFragmentRect) << ",\n"
        << "  \"caret_record_bytes\": " << sizeof(ViewportCaretEdge) << ",\n"
        << "  \"selection_record_bytes\": " << sizeof(ViewportSelectionRect) << ",\n"
        << "  \"retained_bytes\": " << snapshot.current_bytes << ",\n"
        << "  \"peak_bytes\": " << snapshot.peak_bytes << ",\n"
        << "  \"hard_limit_bytes\": " << snapshot.hard_limit_bytes << ",\n"
        << "  \"document_block_extent\": " << output.document_block_extent << ",\n"
        << "  \"hit_queries_per_iteration\": " << kHitQueriesPerIteration << ",\n"
        << "  \"checksum\": " << output_checksum << ",\n"
        << "  \"expected_checksum\": " << kExpectedChecksum << ",\n"
        << "  \"p50_ms\": " << percentile(samples, 0.50) << ",\n"
        << "  \"p95_ms\": " << percentile(samples, 0.95) << ",\n"
        << "  \"p99_ms\": " << percentile(samples, 0.99) << ",\n"
        << "  \"maximum_ms\": " << samples.back() << ",\n"
        << "  \"within_hard_limits\": "
        << (within_hard_limits ? "true" : "false") << ",\n"
        << "  \"accounting_clean\": "
        << (accounting_clean ? "true" : "false") << "\n"
        << "}\n";

    return output_checksum == kExpectedChecksum &&
                   output.lines.size() == kProjectedLines &&
                   output.fragment_rects.size() == kProjectedFragments &&
                   output.carets.size() == kProjectedCarets &&
                   output.selection_rects.size() == kProjectedSelectionRects &&
                   snapshot.current_bytes == kRetainedBytes &&
                   snapshot.peak_bytes == kRetainedBytes &&
                   within_hard_limits && accounting_clean
        ? 0
        : 1;
}
