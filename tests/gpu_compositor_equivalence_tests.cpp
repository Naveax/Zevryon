#include "gpu_compositor_submission.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {
using namespace zevryon::text;

bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

std::uint64_t payload_checksum(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const std::byte value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

GlyphRasterFormat format_for(std::uint32_t page) {
    return page % 3U == 0U ? GlyphRasterFormat::Alpha8
        : (page % 3U == 1U ? GlyphRasterFormat::LcdRgb8
                           : GlyphRasterFormat::Bgra8);
}

GlyphRasterMode mode_for(GlyphRasterFormat format) {
    return format == GlyphRasterFormat::Alpha8 ? GlyphRasterMode::Grayscale
        : (format == GlyphRasterFormat::LcdRgb8 ? GlyphRasterMode::Lcd
                                                : GlyphRasterMode::Color);
}

struct CaseFixture final {
    TextPaintCommandStream paint;
    GlyphRasterWorkingSet working;
    std::vector<GlyphRasterSourceRecord> sources;
    std::vector<std::byte> payload;
    GlyphAtlasCache atlas_cache;
    GlyphAtlasSubmission atlas;
    GlyphAtlasUploadExecution execution;

    CaseFixture(
        std::uint32_t selection_count,
        std::uint32_t page_count,
        bool caret_enabled)
        : atlas_cache(
              {4U, 4U, page_count, page_count, 0U, 0U},
              1U << 20U) {
        paint.clips.push_back({0, 0, 2'048U, 2'048U});
        for (std::uint32_t index = 0U; index < selection_count; ++index) {
            paint.fill_rects.push_back({
                static_cast<std::int64_t>(index * 10U),
                0,
                10U,
                20U,
                1U,
                index,
                index,
                0U});
            paint.commands.push_back({
                TextPaintCommandKind::SelectionRect,
                index,
                0U,
                0U});
        }
        paint.commands.push_back({
            TextPaintCommandKind::GlyphBatch,
            0U,
            0U,
            0U});
        if (caret_enabled) {
            const std::uint32_t fill_index = static_cast<std::uint32_t>(
                paint.fill_rects.size());
            paint.fill_rects.push_back({100, 0, 2U, 20U, 2U, 0U, 0U, 0U});
            paint.commands.push_back({
                TextPaintCommandKind::CaretRect,
                fill_index,
                0U,
                0U});
        }

        sources.reserve(page_count);
        for (std::uint32_t page = 0U; page < page_count; ++page) {
            const GlyphRasterFormat format = format_for(page);
            GlyphRasterWorkingSetEntry entry;
            entry.key.font_generation_id = 17U;
            entry.key.face_id = 3U;
            entry.key.glyph_id = page + 1U;
            entry.key.x_scale = 64;
            entry.key.y_scale = 64;
            entry.key.mode = mode_for(format);
            entry.first_use_index = page;
            entry.use_count = 1U;
            working.entries.push_back(entry);

            GlyphRasterUseRecord use;
            use.viewport_inline_origin = static_cast<std::int64_t>(page * 20U);
            use.viewport_baseline_origin = 20;
            use.key_index = page;
            use.style_id = page;
            working.uses.push_back(use);

            GlyphRasterSourceRecord source;
            source.key = entry.key;
            source.width = 4U;
            source.height = 4U;
            source.format = format;
            const std::uint32_t bytes_per_pixel =
                format == GlyphRasterFormat::Alpha8 ? 1U
                : (format == GlyphRasterFormat::LcdRgb8 ? 3U : 4U);
            source.row_bytes = 4U * bytes_per_pixel;
            source.payload_offset = payload.size();
            source.payload_size =
                static_cast<std::uint64_t>(source.row_bytes) * 4U;
            for (std::uint64_t byte = 0U; byte < source.payload_size; ++byte) {
                payload.push_back(static_cast<std::byte>(
                    (page * 37U + static_cast<std::uint32_t>(byte)) & 0xffU));
            }
            source.content_checksum = payload_checksum(
                std::span<const std::byte>(payload).subspan(
                    static_cast<std::size_t>(source.payload_offset),
                    static_cast<std::size_t>(source.payload_size)));
            sources.push_back(source);
        }

        GlyphAtlasSubmissionError atlas_error;
        const GlyphAtlasSubmissionRequest atlas_request{
            &working,
            sources,
            payload,
            {page_count, payload.size(), page_count, page_count}};
        if (!prepare_glyph_atlas_submission(
                atlas_request,
                &atlas_cache,
                &atlas,
                nullptr,
                &atlas_error)) {
            std::cerr << atlas_error.message << '\n';
            std::abort();
        }
        if (atlas.uploads.size() != page_count ||
            atlas.draw_instances.size() != page_count ||
            atlas.draw_batches.size() != page_count) {
            std::abort();
        }

        ReferenceGlyphAtlasUploadBackend upload_backend;
        GlyphAtlasUploadExecutionError upload_error;
        const GlyphAtlasUploadExecutionRequest upload_request{
            &atlas,
            &atlas_cache,
            payload,
            {page_count, payload.size()}};
        if (!execute_glyph_atlas_uploads(
                upload_request,
                &upload_backend,
                &execution,
                nullptr,
                &upload_error)) {
            std::cerr << upload_error.message << '\n';
            std::abort();
        }
    }
};

bool run_case(
    std::uint32_t selection_count,
    std::uint32_t page_count,
    bool caret_enabled,
    std::uint64_t completed_fence) {
    CaseFixture fixture(selection_count, page_count, caret_enabled);
    ReferenceGpuCompositorBackend backend;
    GpuTextureResidencyCache cache(
        {4U, 4U, page_count, 2U, 3U},
        1U << 20U);
    GpuCompositorFrameRequest request;
    request.paint_stream = &fixture.paint;
    request.atlas_submission = &fixture.atlas;
    request.upload_execution = &fixture.execution;
    request.atlas_cache = &fixture.atlas_cache;
    request.raster_payload = fixture.payload;
    request.frame_generation = 77U;
    request.completed_upload_fence = completed_fence;
    request.limits = {
        page_count,
        page_count,
        selection_count + (caret_enabled ? 1U : 0U),
        selection_count + page_count + (caret_enabled ? 1U : 0U)};

    GpuCompositorFrame frame;
    GpuCompositorFrameStats stats;
    GpuCompositorFrameError error;
    if (!require(
            prepare_gpu_compositor_frame(
                request,
                &cache,
                &backend,
                &frame,
                &stats,
                &error),
            error.message.c_str())) {
        return false;
    }
    const std::size_t expected_commands =
        selection_count + page_count + (caret_enabled ? 1U : 0U);
    if (!require(frame.commands.size() == expected_commands, "command count") ||
        !require(frame.glyph_draws.size() == page_count, "draw count") ||
        !require(frame.texture_uploads.size() == page_count, "upload count") ||
        !require(
            frame.fill_rects.size() ==
                selection_count + (caret_enabled ? 1U : 0U),
            "fill count")) {
        return false;
    }
    for (std::uint32_t index = 0U; index < selection_count; ++index) {
        if (!require(
                frame.commands[index].kind ==
                    GpuCompositorCommandKind::SelectionFill,
                "selection command partition")) {
            return false;
        }
    }
    for (std::uint32_t page = 0U; page < page_count; ++page) {
        const std::size_t command_index = selection_count + page;
        const GpuGlyphDrawPacket& draw = frame.glyph_draws[page];
        if (!require(
                frame.commands[command_index].kind ==
                    GpuCompositorCommandKind::GlyphDraw,
                "glyph command partition") ||
            !require(draw.texture.page_index == page, "draw texture page") ||
            !require(draw.required_fence_value == 0U,
                     "new texture has no submitted GPU fence") ||
            !require(
                (draw.flags & kGpuGlyphDrawRequiresUploadWait) != 0U,
                "pending texture wait flag")) {
            return false;
        }
    }
    if (caret_enabled &&
        !require(
            frame.commands.back().kind == GpuCompositorCommandKind::CaretFill,
            "caret command partition")) {
        return false;
    }
    if (!require(frame.required_upload_fence == 0U,
                 "new uploads require no prior GPU fence") ||
        !require(stats.pending_textures == page_count,
                 "new textures remain pending until frame submission") ||
        !require(stats.resident_textures == 0U,
                 "new textures are not prematurely resident")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::uint64_t passed = 0U;
    for (std::uint32_t selections = 0U; selections <= 3U; ++selections) {
        for (std::uint32_t pages = 1U; pages <= 4U; ++pages) {
            for (std::uint32_t caret = 0U; caret <= 1U; ++caret) {
                for (std::uint64_t completed = 0U; completed <= 5U; ++completed) {
                    if (!run_case(
                            selections,
                            pages,
                            caret != 0U,
                            completed)) {
                        return 1;
                    }
                    ++passed;
                }
            }
        }
    }
    constexpr std::uint64_t kExpectedCases = 192U;
    if (!require(passed == kExpectedCases, "exact oracle case count")) {
        return 1;
    }
    std::cout << "gpu-compositor-equivalence: " << passed << "/"
              << kExpectedCases << " PASS\n";
    return 0;
}
