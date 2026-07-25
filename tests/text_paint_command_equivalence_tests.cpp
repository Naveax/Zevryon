#include "text_paint_command_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <string>

namespace {
using namespace zevryon::text;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

void configure_segment(
    MultiRunShapedSegment* segment,
    std::uint32_t first_cluster,
    std::uint32_t cluster_limit,
    FontFaceId face_id,
    ShapingDirection direction) {
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
    segment->glyphs.x_scale = 64;
    segment->glyphs.y_scale = 64;
}

bool certify_ltr_partition(
    std::uint32_t cluster_count,
    std::uint32_t partition_mask,
    bool split_segments) {
    MultiRunShapedText shaped;
    GlyphClusterMap clusters;
    LineFragmentLayout fragments;
    ViewportProjection projection;

    std::uint32_t first_cluster = 0U;
    std::uint32_t fragment_index = 0U;
    std::uint32_t segment_index = 0U;
    std::uint64_t inline_offset = 0U;

    auto emit_chunk = [&](std::uint32_t limit) {
        const std::uint32_t chunk_count = limit - first_cluster;
        if (!split_segments && shaped.segments.empty()) {
            shaped.segments.emplace_back(std::pmr::get_default_resource());
            configure_segment(
                &shaped.segments.back(),
                0U,
                cluster_count,
                1U,
                ShapingDirection::LeftToRight);
            for (std::uint32_t cluster = 0U;
                 cluster < cluster_count;
                 ++cluster) {
                shaped.segments.back().glyphs.glyphs.push_back(
                    {cluster + 1U, cluster, 10, 0, 0, 0, 0U});
            }
        }
        if (split_segments) {
            shaped.segments.emplace_back(std::pmr::get_default_resource());
            configure_segment(
                &shaped.segments.back(),
                first_cluster,
                limit,
                segment_index + 1U,
                ShapingDirection::LeftToRight);
            for (std::uint32_t cluster = first_cluster;
                 cluster < limit;
                 ++cluster) {
                shaped.segments.back().glyphs.glyphs.push_back(
                    {cluster + 1U,
                     cluster,
                     10,
                     0,
                     0,
                     0,
                     0U});
            }
        }

        const std::uint32_t active_segment =
            split_segments ? segment_index : 0U;
        for (std::uint32_t cluster = first_cluster;
             cluster < limit;
             ++cluster) {
            const std::uint32_t first_glyph =
                split_segments
                    ? cluster - first_cluster
                    : cluster;
            clusters.records.push_back(
                {active_segment,
                 cluster,
                 first_glyph,
                 1U});
        }
        fragments.fragments.push_back({
            inline_offset,
            static_cast<std::uint64_t>(chunk_count) * 10U,
            active_segment,
            first_cluster,
            limit,
            0U,
            0U,
            0U});
        projection.fragment_rects.push_back({
            static_cast<std::int64_t>(inline_offset),
            0,
            static_cast<std::uint64_t>(chunk_count) * 10U,
            100U,
            fragment_index,
            first_cluster,
            limit,
            0U});
        inline_offset +=
            static_cast<std::uint64_t>(chunk_count) * 10U;
        first_cluster = limit;
        ++fragment_index;
        ++segment_index;
    };

    for (std::uint32_t boundary = 1U;
         boundary < cluster_count;
         ++boundary) {
        if ((partition_mask &
             (1U << (boundary - 1U))) != 0U) {
            emit_chunk(boundary);
        }
    }
    emit_chunk(cluster_count);

    fragments.lines.push_back({
        inline_offset,
        0U,
        fragment_index,
        cluster_count,
        0U});
    projection.lines.push_back({
        0,
        80,
        100U,
        inline_offset,
        0U,
        0U,
        fragment_index,
        0U,
        0U,
        0U,
        0U,
        0U});

    TextPaintCommandStreamRequest request;
    request.projection = &projection;
    request.fragment_layout = &fragments;
    request.shaped_text = &shaped;
    request.cluster_map = &clusters;
    request.default_text_style_id = 9U;
    request.clip_inline_size = inline_offset + 1U;
    request.clip_block_size = 100U;
    request.paint_selection = false;
    request.limits = {
        fragment_index + 1U,
        fragment_index + 1U,
        1U,
        cluster_count};

    TextPaintCommandStream output;
    TextPaintCommandStreamStats stats;
    TextPaintCommandStreamError error;
    if (!build_text_paint_command_stream(
            request,
            &output,
            &stats,
            &error)) {
        std::cerr << "build failed for n=" << cluster_count
                  << " mask=" << partition_mask
                  << " split=" << split_segments
                  << " kind="
                  << text_paint_command_stream_error_kind_name(
                         error.kind)
                  << '\n';
        return false;
    }

    const std::uint32_t expected_batches =
        split_segments ? fragment_index : 1U;
    if (output.glyph_batches.size() != expected_batches ||
        output.commands.size() != expected_batches ||
        stats.referenced_glyphs != cluster_count) {
        return false;
    }
    if (!split_segments) {
        const auto& batch = output.glyph_batches.front();
        return batch.first_glyph == 0U &&
               batch.glyph_count == cluster_count &&
               batch.source_fragment_count == fragment_index &&
               batch.viewport_inline_origin == 0 &&
               (fragment_index == 1U ||
                (batch.flags &
                 kTextPaintGlyphBatchCoalesced) != 0U);
    }

    std::uint32_t expected_first_cluster = 0U;
    for (std::uint32_t index = 0U;
         index < fragment_index;
         ++index) {
        const InlineLayoutFragment& fragment =
            fragments.fragments[index];
        const TextPaintGlyphBatch& batch =
            output.glyph_batches[index];
        const std::uint32_t expected_count =
            fragment.cluster_limit -
            fragment.first_cluster;
        if (fragment.first_cluster != expected_first_cluster ||
            batch.segment_index != index ||
            batch.first_glyph != 0U ||
            batch.glyph_count != expected_count ||
            batch.source_fragment_count != 1U) {
            return false;
        }
        expected_first_cluster = fragment.cluster_limit;
    }
    return true;
}

bool certify_rtl_span(std::uint32_t cluster_count) {
    MultiRunShapedText shaped;
    GlyphClusterMap clusters;
    LineFragmentLayout fragments;
    ViewportProjection projection;

    shaped.segments.emplace_back(std::pmr::get_default_resource());
    configure_segment(
        &shaped.segments.back(),
        0U,
        cluster_count,
        7U,
        ShapingDirection::RightToLeft);
    for (std::uint32_t glyph = 0U;
         glyph < cluster_count;
         ++glyph) {
        const std::uint32_t cluster =
            cluster_count - 1U - glyph;
        shaped.segments.back().glyphs.glyphs.push_back(
            {glyph + 1U, cluster, -10, 0, 0, 0, 0U});
    }
    for (std::uint32_t cluster = 0U;
         cluster < cluster_count;
         ++cluster) {
        clusters.records.push_back({
            0U,
            cluster,
            cluster_count - 1U - cluster,
            1U});
    }

    const std::uint64_t advance =
        static_cast<std::uint64_t>(cluster_count) * 10U;
    fragments.fragments.push_back({
        5U,
        advance,
        0U,
        0U,
        cluster_count,
        1U,
        static_cast<std::uint8_t>(
            kInlineFragmentGlyphRunRtl),
        0U});
    fragments.lines.push_back({
        advance,
        0U,
        1U,
        cluster_count,
        kVisualLineContainsRtl});
    projection.fragment_rects.push_back({
        5,
        0,
        advance,
        100U,
        0U,
        0U,
        cluster_count,
        kViewportFragmentRtl});
    projection.lines.push_back({
        0,
        80,
        100U,
        advance,
        0U,
        0U,
        1U,
        0U,
        0U,
        0U,
        0U,
        kViewportLineContainsRtl});

    TextPaintCommandStreamRequest request;
    request.projection = &projection;
    request.fragment_layout = &fragments;
    request.shaped_text = &shaped;
    request.cluster_map = &clusters;
    request.default_text_style_id = 4U;
    request.clip_inline_size = advance + 10U;
    request.clip_block_size = 100U;
    request.paint_selection = false;
    request.limits = {2U, 2U, 1U, cluster_count};

    TextPaintCommandStream output;
    TextPaintCommandStreamStats stats;
    TextPaintCommandStreamError error;
    if (!build_text_paint_command_stream(
            request,
            &output,
            &stats,
            &error) ||
        output.glyph_batches.size() != 1U) {
        return false;
    }
    const TextPaintGlyphBatch& batch =
        output.glyph_batches.front();
    return batch.first_glyph == 0U &&
           batch.glyph_count == cluster_count &&
           batch.viewport_inline_origin ==
               static_cast<std::int64_t>(advance + 5U) &&
           (batch.flags & kTextPaintGlyphBatchRtl) != 0U;
}

} // namespace

int main() {
    std::uint64_t cases = 0U;
    for (std::uint32_t count = 1U; count <= 12U; ++count) {
        const std::uint32_t masks =
            1U << (count - 1U);
        for (std::uint32_t mask = 0U;
             mask < masks;
             ++mask) {
            if (!require(
                    certify_ltr_partition(
                        count,
                        mask,
                        false),
                    "single-segment LTR partition") ||
                !require(
                    certify_ltr_partition(
                        count,
                        mask,
                        true),
                    "multi-segment LTR partition")) {
                return 1;
            }
            cases += 2U;
        }
    }
    for (std::uint32_t count = 1U; count <= 32U; ++count) {
        if (!require(
                certify_rtl_span(count),
                "RTL zero-copy span")) {
            return 1;
        }
        ++cases;
    }
    if (!require(cases == 8222U, "expected oracle case count")) {
        return 1;
    }
    std::cout << "text paint command equivalence cases: "
              << cases << '\n';
    return 0;
}
