#include "text_paint_command_stream.hpp"

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
constexpr std::uint32_t kProjectedLines = 80U;
constexpr std::uint32_t kProjectedFragments =
    kProjectedLines * kFragmentsPerLine;
constexpr std::uint32_t kProjectedCarets =
    kProjectedLines * 8U;
constexpr std::uint32_t kSelectionRects = 64U;
constexpr std::uint32_t kGlyphBatches =
    kProjectedLines * 3U;
constexpr std::uint32_t kFillRects =
    kSelectionRects + 1U;
constexpr std::uint32_t kCommands =
    kSelectionRects + kGlyphBatches + 1U;
constexpr std::uint64_t kReferencedGlyphs =
    kProjectedLines * kFragmentsPerLine;
constexpr std::size_t kRetainedBytes =
    sizeof(TextPaintClipRect) +
    kCommands * sizeof(TextPaintCommandRecord) +
    kGlyphBatches * sizeof(TextPaintGlyphBatch) +
    kFillRects * sizeof(TextPaintFillRect);
constexpr std::uint64_t kExpectedChecksum =
    1'355'758'546'038'770'003ULL;
constexpr std::uint32_t kMeasuredIterations = 400U;

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

void configure_segment(
    MultiRunShapedSegment* segment,
    std::uint32_t first_cluster,
    std::uint32_t cluster_limit,
    FontFaceId face_id,
    ShapingDirection direction,
    std::int32_t scale) {
    segment->run.cluster_index = first_cluster;
    segment->run.face_id = face_id;
    segment->run.script = ScriptId::Latn;
    segment->run.direction = direction;
    segment->run.fallback_source = FontFallbackSource::Primary;
    segment->run.bidi_level =
        direction == ShapingDirection::RightToLeft ? 1U : 0U;
    segment->glyphs.first_cluster = first_cluster;
    segment->glyphs.cluster_limit = cluster_limit;
    segment->glyphs.script = ScriptId::Latn;
    segment->glyphs.direction = direction;
    segment->glyphs.x_scale = scale;
    segment->glyphs.y_scale = scale;
}

struct Fixture final {
    MultiRunShapedText shaped;
    GlyphClusterMap cluster_map;
    LineFragmentLayout fragments;
    ViewportProjection projection;
    std::vector<std::uint32_t> styles;

    Fixture() {
        shaped.segments.emplace_back(std::pmr::get_default_resource());
        configure_segment(
            &shaped.segments.back(),
            0U,
            kClusters,
            1U,
            ShapingDirection::LeftToRight,
            64);
        shaped.segments.back().glyphs.glyphs.push_back(
            {1U, 0U, 10, 0, 0, 0, 0U});
        shaped.segments.back().glyphs.glyphs.push_back(
            {2U, 1U, 10, 0, 0, 0, 0U});

        shaped.segments.emplace_back(std::pmr::get_default_resource());
        configure_segment(
            &shaped.segments.back(),
            0U,
            kClusters,
            2U,
            ShapingDirection::RightToLeft,
            72);
        shaped.segments.back().glyphs.glyphs.push_back(
            {3U, 3U, -10, 0, 0, 0, 0U});
        shaped.segments.back().glyphs.glyphs.push_back(
            {4U, 2U, -10, 0, 0, 0, 0U});

        styles = {10U, 20U};
        cluster_map.records.reserve(kClusters);
        fragments.lines.reserve(kDocumentLines);
        fragments.fragments.reserve(kClusters);

        for (std::uint32_t line = 0U;
             line < kDocumentLines;
             ++line) {
            const std::uint32_t cluster = line * 4U;
            const std::uint32_t fragment = line * 4U;
            cluster_map.records.push_back(
                {0U, cluster, 0U, 1U});
            cluster_map.records.push_back(
                {0U, cluster + 1U, 1U, 1U});
            cluster_map.records.push_back(
                {1U, cluster + 2U, 1U, 1U});
            cluster_map.records.push_back(
                {1U, cluster + 3U, 0U, 1U});

            fragments.fragments.push_back(
                {0U, 10U, 0U, cluster, cluster + 1U, 0U, 0U, 0U});
            fragments.fragments.push_back(
                {10U, 10U, 0U, cluster + 1U, cluster + 2U, 0U, 0U, 0U});
            fragments.fragments.push_back(
                {20U,
                 10U,
                 1U,
                 cluster + 2U,
                 cluster + 3U,
                 1U,
                 static_cast<std::uint8_t>(
                     kInlineFragmentGlyphRunRtl),
                 0U});
            fragments.fragments.push_back(
                {30U,
                 10U,
                 1U,
                 cluster + 3U,
                 cluster + 4U,
                 1U,
                 static_cast<std::uint8_t>(
                     kInlineFragmentGlyphRunRtl),
                 0U});
            fragments.lines.push_back(
                {40U,
                 fragment,
                 4U,
                 cluster + 4U,
                 kVisualLineContainsRtl});
        }

        projection.viewport_inline_start = 0U;
        projection.viewport_block_start =
            8'192ULL * 1'000U;
        projection.document_block_extent =
            static_cast<std::uint64_t>(kDocumentLines) * 1'000U;
        projection.lines.reserve(kProjectedLines);
        projection.fragment_rects.reserve(kProjectedFragments);
        projection.carets.reserve(kProjectedCarets);
        projection.selection_rects.reserve(kSelectionRects);

        for (std::uint32_t offset = 0U;
             offset < kProjectedLines;
             ++offset) {
            const std::uint32_t source_line =
                8'184U + offset;
            const std::uint32_t cluster =
                source_line * 4U;
            const std::uint32_t source_fragment =
                source_line * 4U;
            const std::int64_t block_start =
                static_cast<std::int64_t>(offset) * 1'000 - 8'000;
            const std::uint32_t first_rect =
                static_cast<std::uint32_t>(
                    projection.fragment_rects.size());
            const std::uint32_t first_caret =
                static_cast<std::uint32_t>(
                    projection.carets.size());
            const std::uint32_t first_selection =
                static_cast<std::uint32_t>(
                    projection.selection_rects.size());

            projection.fragment_rects.push_back(
                {0,
                 block_start,
                 10U,
                 1'000U,
                 source_fragment,
                 cluster,
                 cluster + 1U,
                 0U});
            projection.fragment_rects.push_back(
                {10,
                 block_start,
                 10U,
                 1'000U,
                 source_fragment + 1U,
                 cluster + 1U,
                 cluster + 2U,
                 0U});
            projection.fragment_rects.push_back(
                {20,
                 block_start,
                 10U,
                 1'000U,
                 source_fragment + 2U,
                 cluster + 2U,
                 cluster + 3U,
                 kViewportFragmentRtl});
            projection.fragment_rects.push_back(
                {30,
                 block_start,
                 10U,
                 1'000U,
                 source_fragment + 3U,
                 cluster + 3U,
                 cluster + 4U,
                 kViewportFragmentRtl});

            projection.carets.push_back(
                {0, block_start, 1'000U, cluster, source_fragment, 0U, 0U});
            projection.carets.push_back(
                {10, block_start, 1'000U, cluster + 1U, source_fragment, 0U, 0U});
            projection.carets.push_back(
                {10, block_start, 1'000U, cluster + 1U, source_fragment + 1U, 0U, 0U});
            projection.carets.push_back(
                {20, block_start, 1'000U, cluster + 2U, source_fragment + 1U, 0U, 0U});
            projection.carets.push_back(
                {30, block_start, 1'000U, cluster + 2U, source_fragment + 2U, kViewportCaretRtl, 0U});
            projection.carets.push_back(
                {20, block_start, 1'000U, cluster + 3U, source_fragment + 2U, kViewportCaretRtl, 0U});
            projection.carets.push_back(
                {40, block_start, 1'000U, cluster + 3U, source_fragment + 3U, kViewportCaretRtl, 0U});
            projection.carets.push_back(
                {30, block_start, 1'000U, cluster + 4U, source_fragment + 3U, kViewportCaretRtl, 0U});

            std::uint32_t selection_count = 0U;
            if (offset >= 8U && offset < 72U) {
                projection.selection_rects.push_back(
                    {0,
                     block_start,
                     10U,
                     1'000U,
                     source_line,
                     source_fragment,
                     0U,
                     0U});
                selection_count = 1U;
            }

            std::uint32_t flags =
                kViewportLineContainsRtl;
            if (offset < 8U) {
                flags |= kViewportLineBeforeViewport;
            } else if (offset >= 72U) {
                flags |= kViewportLineAfterViewport;
            }
            if (selection_count != 0U) {
                flags |= kViewportLineContainsSelection;
            }
            projection.lines.push_back(
                {block_start,
                 block_start + 800,
                 1'000U,
                 40U,
                 source_line,
                 first_rect,
                 4U,
                 first_caret,
                 8U,
                 first_selection,
                 selection_count,
                 flags});
        }
    }

    TextPaintCommandStreamRequest request() const {
        TextPaintCommandStreamRequest value;
        value.projection = &projection;
        value.fragment_layout = &fragments;
        value.shaped_text = &shaped;
        value.cluster_map = &cluster_map;
        value.segment_style_ids = styles;
        value.selection_style_id = 30U;
        value.caret_style_id = 40U;
        value.clip_inline_size = 40U;
        value.clip_block_size = 64'000U;
        value.paint_selection = true;
        const std::uint32_t active_line = 8'192U;
        value.caret = {
            active_line,
            active_line * 4U,
            active_line * 4U,
            0U,
            2U,
            true};
        value.limits = {
            kCommands,
            kGlyphBatches,
            kFillRects,
            kReferencedGlyphs};
        return value;
    }
};

std::uint64_t checksum(const TextPaintCommandStream& stream) {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const TextPaintClipRect& record : stream.clips) {
        mix(&hash, static_cast<std::uint64_t>(record.viewport_inline_start));
        mix(&hash, static_cast<std::uint64_t>(record.viewport_block_start));
        mix(&hash, record.inline_size);
        mix(&hash, record.block_size);
    }
    for (const TextPaintCommandRecord& record : stream.commands) {
        mix(&hash, static_cast<std::uint32_t>(record.kind));
        mix(&hash, record.payload_index);
        mix(&hash, record.clip_index);
        mix(&hash, record.flags);
    }
    for (const TextPaintGlyphBatch& record : stream.glyph_batches) {
        mix(&hash, static_cast<std::uint64_t>(record.viewport_inline_origin));
        mix(&hash, static_cast<std::uint64_t>(record.viewport_baseline));
        mix(&hash, record.segment_index);
        mix(&hash, record.first_glyph);
        mix(&hash, record.glyph_count);
        mix(&hash, record.style_id);
        mix(&hash, record.face_id);
        mix(&hash, static_cast<std::uint32_t>(record.x_scale));
        mix(&hash, static_cast<std::uint32_t>(record.y_scale));
        mix(&hash, record.source_line_index);
        mix(&hash, record.first_source_fragment_index);
        mix(&hash, record.source_fragment_count);
        mix(&hash, record.flags);
    }
    for (const TextPaintFillRect& record : stream.fill_rects) {
        mix(&hash, static_cast<std::uint64_t>(record.viewport_inline_start));
        mix(&hash, static_cast<std::uint64_t>(record.viewport_block_start));
        mix(&hash, record.inline_size);
        mix(&hash, record.block_size);
        mix(&hash, record.style_id);
        mix(&hash, record.source_line_index);
        mix(&hash, record.source_fragment_index);
        mix(&hash, record.flags);
    }
    return hash;
}

} // namespace

int main() {
    using namespace zevryon;
    Fixture fixture;
    const TextPaintCommandStreamRequest request =
        fixture.request();

    core::ResourceLedger ledger;
    ledger.set_hard_limit(
        core::ResourceClass::PaintCommand,
        kRetainedBytes);
    core::LedgerMemoryResource resource(
        ledger,
        core::ResourceClass::PaintCommand);
    TextPaintCommandStream output(&resource);
    TextPaintCommandStreamStats stats;
    TextPaintCommandStreamError error;

    if (!build_text_paint_command_stream(
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
        const auto start =
            std::chrono::steady_clock::now();
        if (!build_text_paint_command_stream(
                request,
                &output,
                &stats,
                &error)) {
            std::cerr << error.message << '\n';
            return 1;
        }
        const auto stop =
            std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(
                stop - start).count());
    }
    std::sort(samples.begin(), samples.end());

    const core::ResourceSnapshot snapshot =
        ledger.snapshot(core::ResourceClass::PaintCommand);
    const std::uint64_t output_checksum = checksum(output);
    const bool within_hard_limits =
        ledger.within_hard_limits();
    const bool accounting_clean =
        ledger.accounting_clean();

    std::cout
        << "{\n"
        << "  \"schema\": \"zevryon.text-paint-command-stream-benchmark.v1\",\n"
        << "  \"input_document_lines\": " << kDocumentLines << ",\n"
        << "  \"input_clusters\": " << kClusters << ",\n"
        << "  \"input_source_fragments\": " << kClusters << ",\n"
        << "  \"input_projected_lines\": " << stats.input_lines << ",\n"
        << "  \"input_projected_fragment_rects\": "
        << stats.input_fragment_rects << ",\n"
        << "  \"input_projected_carets\": " << stats.input_carets << ",\n"
        << "  \"input_projected_selection_rects\": "
        << stats.input_selection_rects << ",\n"
        << "  \"output_clips\": " << output.clips.size() << ",\n"
        << "  \"output_commands\": " << output.commands.size() << ",\n"
        << "  \"output_glyph_batches\": "
        << output.glyph_batches.size() << ",\n"
        << "  \"output_fill_rects\": " << output.fill_rects.size() << ",\n"
        << "  \"selection_commands\": " << stats.selection_commands << ",\n"
        << "  \"caret_commands\": " << stats.caret_commands << ",\n"
        << "  \"referenced_glyphs\": " << stats.referenced_glyphs << ",\n"
        << "  \"coalesced_fragments\": " << stats.coalesced_fragments << ",\n"
        << "  \"rtl_glyph_batches\": " << stats.rtl_glyph_batches << ",\n"
        << "  \"lines_before_viewport\": " << stats.lines_before_viewport << ",\n"
        << "  \"lines_after_viewport\": " << stats.lines_after_viewport << ",\n"
        << "  \"maximum_glyphs_per_batch\": "
        << stats.maximum_glyphs_per_batch << ",\n"
        << "  \"maximum_glyph_batches_per_line\": "
        << stats.maximum_glyph_batches_per_line << ",\n"
        << "  \"clip_record_bytes\": " << sizeof(TextPaintClipRect) << ",\n"
        << "  \"command_record_bytes\": "
        << sizeof(TextPaintCommandRecord) << ",\n"
        << "  \"glyph_batch_record_bytes\": "
        << sizeof(TextPaintGlyphBatch) << ",\n"
        << "  \"fill_rect_record_bytes\": "
        << sizeof(TextPaintFillRect) << ",\n"
        << "  \"retained_bytes\": " << snapshot.current_bytes << ",\n"
        << "  \"peak_bytes\": " << snapshot.peak_bytes << ",\n"
        << "  \"hard_limit_bytes\": " << snapshot.hard_limit_bytes << ",\n"
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
                   output.clips.size() == 1U &&
                   output.commands.size() == kCommands &&
                   output.glyph_batches.size() == kGlyphBatches &&
                   output.fill_rects.size() == kFillRects &&
                   stats.selection_commands == kSelectionRects &&
                   stats.caret_commands == 1U &&
                   stats.referenced_glyphs == kReferencedGlyphs &&
                   stats.coalesced_fragments == kProjectedLines &&
                   stats.rtl_glyph_batches == kProjectedLines * 2U &&
                   snapshot.current_bytes == kRetainedBytes &&
                   snapshot.peak_bytes == kRetainedBytes &&
                   within_hard_limits &&
                   accounting_clean
        ? 0
        : 1;
}
