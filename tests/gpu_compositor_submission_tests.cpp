#include "gpu_compositor_submission.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <new>
#include <span>
#include <vector>

namespace {
using namespace zevryon::text;

class FailingMemoryResource final : public std::pmr::memory_resource {
public:
    explicit FailingMemoryResource(std::size_t limit) : limit_(limit) {}
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > limit_ - used_) {
            throw std::bad_alloc();
        }
        void* value = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        used_ += bytes;
        return value;
    }
    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        used_ -= bytes;
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t limit_{0};
    std::size_t used_{0};
};

std::uint64_t payload_checksum(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const std::byte value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

GlyphRasterMode mode_for_format(GlyphRasterFormat format) {
    switch (format) {
        case GlyphRasterFormat::Alpha8: return GlyphRasterMode::Grayscale;
        case GlyphRasterFormat::LcdRgb8: return GlyphRasterMode::Lcd;
        case GlyphRasterFormat::Bgra8: return GlyphRasterMode::Color;
        case GlyphRasterFormat::Empty: break;
    }
    return GlyphRasterMode::Grayscale;
}

struct Fixture final {
    TextPaintCommandStream paint;
    GlyphRasterWorkingSet working;
    std::vector<GlyphRasterSourceRecord> sources;
    std::vector<std::byte> payload;
    GlyphAtlasCache atlas_cache;
    GlyphAtlasSubmission atlas;
    GlyphAtlasUploadExecution uploads;

    explicit Fixture(std::span<const GlyphRasterFormat> formats)
        : atlas_cache(
              {128U, 128U, 3U, 16U, 1U, 0U},
              1U << 20U) {
        paint.clips.push_back({0, 0, 1'024U, 1'000U});
        paint.fill_rects.push_back({0, 0, 100U, 20U, 10U, 1U, 2U, 0U});
        paint.fill_rects.push_back({90, 0, 2U, 20U, 11U, 1U, 2U, 0U});
        paint.commands.push_back({TextPaintCommandKind::SelectionRect, 0U, 0U, 0U});
        paint.commands.push_back({TextPaintCommandKind::GlyphBatch, 0U, 0U, 0U});
        paint.commands.push_back({TextPaintCommandKind::CaretRect, 1U, 0U, 0U});

        sources.reserve(formats.size());
        for (std::uint32_t index = 0U; index < formats.size(); ++index) {
            const GlyphRasterFormat format = formats[index];
            GlyphRasterWorkingSetEntry entry;
            entry.key.font_generation_id = 7U;
            entry.key.face_id = 3U;
            entry.key.glyph_id = index + 1U;
            entry.key.x_scale = 64;
            entry.key.y_scale = 64;
            entry.key.mode = mode_for_format(format);
            entry.first_use_index = index;
            entry.use_count = 1U;
            working.entries.push_back(entry);

            GlyphRasterUseRecord use;
            use.viewport_inline_origin = static_cast<std::int64_t>(index * 20U);
            use.viewport_baseline_origin = 50;
            use.key_index = index;
            use.style_id = 20U + index;
            use.source_line_index = 1U;
            working.uses.push_back(use);

            GlyphRasterSourceRecord source;
            source.key = entry.key;
            source.width = 4U;
            source.height = 4U;
            source.bearing_y = 3;
            source.format = format;
            const std::uint32_t bytes_per_pixel =
                format == GlyphRasterFormat::Alpha8 ? 1U
                : (format == GlyphRasterFormat::LcdRgb8 ? 3U : 4U);
            source.row_bytes = source.width * bytes_per_pixel;
            source.payload_offset = payload.size();
            source.payload_size =
                static_cast<std::uint64_t>(source.row_bytes) * source.height;
            for (std::uint64_t byte = 0U; byte < source.payload_size; ++byte) {
                payload.push_back(static_cast<std::byte>(
                    (index * 31U + static_cast<std::uint32_t>(byte)) & 0xffU));
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
            {static_cast<std::uint32_t>(formats.size()),
             payload.size(),
             formats.size(),
             static_cast<std::uint32_t>(formats.size())}};
        assert(prepare_glyph_atlas_submission(
            atlas_request,
            &atlas_cache,
            &atlas,
            nullptr,
            &atlas_error));

        ReferenceGlyphAtlasUploadBackend upload_backend;
        GlyphAtlasUploadExecutionError upload_error;
        const GlyphAtlasUploadExecutionRequest upload_request{
            &atlas,
            &atlas_cache,
            payload,
            {static_cast<std::uint32_t>(formats.size()), payload.size()}};
        assert(execute_glyph_atlas_uploads(
            upload_request,
            &upload_backend,
            &uploads,
            nullptr,
            &upload_error));
    }

    static Fixture three_pages() {
        const std::array<GlyphRasterFormat, 3> formats{
            GlyphRasterFormat::Alpha8,
            GlyphRasterFormat::LcdRgb8,
            GlyphRasterFormat::Bgra8};
        return Fixture(formats);
    }

    static Fixture one_page(GlyphRasterFormat format) {
        const std::array<GlyphRasterFormat, 1> formats{format};
        return Fixture(formats);
    }

    GpuCompositorFrameRequest request() const {
        GpuCompositorFrameRequest request;
        request.paint_stream = &paint;
        request.atlas_submission = &atlas;
        request.upload_execution = &uploads;
        request.atlas_cache = &atlas_cache;
        request.raster_payload = payload;
        request.frame_generation = 41U;
        request.completed_upload_fence = uploads.last_fence_value;
        request.limits = {
            static_cast<std::uint32_t>(uploads.batches.size()),
            static_cast<std::uint32_t>(atlas.draw_batches.size()),
            2U,
            static_cast<std::uint32_t>(atlas.draw_batches.size() + 2U)};
        return request;
    }
};

void test_cold_frame_and_submission_ring() {
    Fixture fixture = Fixture::three_pages();
    ReferenceGpuCompositorBackend backend;
    GpuTextureResidencyCache cache(
        {256U, 256U, 3U, 2U, 1U},
        64U << 10U);
    GpuCompositorFrame frame;
    GpuCompositorFrameStats stats;
    GpuCompositorFrameError error;
    assert(prepare_gpu_compositor_frame(
        fixture.request(), &cache, &backend, &frame, &stats, &error));
    assert(frame.texture_uploads.size() == 3U);
    assert(frame.glyph_draws.size() == 3U);
    assert(frame.fill_rects.size() == 2U);
    assert(frame.commands.size() == 5U);
    assert(frame.commands.front().kind == GpuCompositorCommandKind::SelectionFill);
    assert(frame.commands[1].kind == GpuCompositorCommandKind::GlyphDraw);
    assert(frame.commands.back().kind == GpuCompositorCommandKind::CaretFill);
    assert(frame.required_upload_fence == 0U);
    assert(stats.allocated_textures == 3U);
    assert(stats.resident_textures == 0U);
    assert(stats.pending_textures == 3U);
    assert(gpu_compositor_frame_is_current(cache, frame));

    GpuFrameReceipt first;
    GpuFrameReceipt second;
    assert(submit_gpu_compositor_frame(frame, &cache, &backend, &first, &error));
    assert(submit_gpu_compositor_frame(frame, &cache, &backend, &second, &error));
    GpuFrameReceipt blocked;
    assert(!submit_gpu_compositor_frame(frame, &cache, &backend, &blocked, &error));
    assert(error.kind == GpuCompositorFrameErrorKind::InFlightCapacityExceeded);
    cache.retire_completed_frames(first.fence_value);
    assert(submit_gpu_compositor_frame(frame, &cache, &backend, &blocked, &error));
    cache.retire_completed_frames(blocked.fence_value);
    const auto snapshot = cache.snapshot();
    assert(snapshot.texture_count == 3U);
    assert(snapshot.retired_frames >= 3U);
}

void test_hot_frame_reuses_textures() {
    Fixture fixture = Fixture::three_pages();
    ReferenceGpuCompositorBackend backend;
    GpuTextureResidencyCache cache(
        {256U, 256U, 3U, 2U, 1U},
        64U << 10U);
    GpuCompositorFrame cold;
    GpuCompositorFrameStats cold_stats;
    GpuCompositorFrameError error;
    auto request = fixture.request();
    assert(prepare_gpu_compositor_frame(
        request, &cache, &backend, &cold, &cold_stats, &error));
    GpuFrameReceipt cold_receipt;
    assert(submit_gpu_compositor_frame(
        cold, &cache, &backend, &cold_receipt, &error));
    cache.retire_completed_frames(cold_receipt.fence_value);

    request.frame_generation = 42U;
    request.completed_upload_fence = cold_receipt.fence_value;
    GpuCompositorFrame hot;
    GpuCompositorFrameStats hot_stats;
    assert(prepare_gpu_compositor_frame(
        request, &cache, &backend, &hot, &hot_stats, &error));
    assert(hot.texture_uploads.empty());
    assert(hot.glyph_draws.size() == 3U);
    assert(hot_stats.allocated_textures == 0U);
    assert(hot_stats.reused_textures == 3U);
    assert(hot_stats.resident_textures == 3U);
    assert(gpu_compositor_frame_is_current(cache, hot));
}

void test_clear_invalidates_frame() {
    Fixture fixture = Fixture::three_pages();
    ReferenceGpuCompositorBackend backend;
    GpuTextureResidencyCache cache(
        {256U, 256U, 3U, 2U, 1U},
        64U << 10U);
    GpuCompositorFrame frame;
    GpuCompositorFrameError error;
    assert(prepare_gpu_compositor_frame(
        fixture.request(), &cache, &backend, &frame, nullptr, &error));
    assert(gpu_compositor_frame_is_current(cache, frame));
    cache.clear(&backend);
    assert(!gpu_compositor_frame_is_current(cache, frame));
    GpuFrameReceipt receipt;
    assert(!submit_gpu_compositor_frame(frame, &cache, &backend, &receipt, &error));
}

void test_pending_zero_fence_is_not_promoted() {
    Fixture fixture = Fixture::one_page(GlyphRasterFormat::Alpha8);
    ReferenceGpuCompositorBackend backend;
    GpuTextureResidencyCache cache(
        {256U, 256U, 1U, 1U, 1U},
        64U << 10U);
    GpuCompositorFrame frame;
    GpuCompositorFrameStats stats;
    GpuCompositorFrameError error;
    assert(prepare_gpu_compositor_frame(
        fixture.request(), &cache, &backend, &frame, &stats, &error));
    assert(stats.pending_textures == 1U);
    assert(stats.resident_textures == 0U);
    cache.retire_completed_frames(std::numeric_limits<std::uint64_t>::max());

    GpuCompositorFrame repeated;
    GpuCompositorFrameStats repeated_stats;
    auto request = fixture.request();
    request.frame_generation = 42U;
    request.completed_upload_fence = std::numeric_limits<std::uint64_t>::max();
    assert(prepare_gpu_compositor_frame(
        request, &cache, &backend, &repeated, &repeated_stats, &error));
    assert(repeated.texture_uploads.size() == 1U);
    assert(repeated_stats.pending_textures == 1U);
    assert(repeated_stats.resident_textures == 0U);
}

void test_safe_lru_eviction() {
    ReferenceGpuCompositorBackend backend;
    GpuTextureResidencyCache cache(
        {256U, 256U, 2U, 1U, 1U},
        64U << 10U);
    GpuCompositorFrameError error;

    Fixture first = Fixture::one_page(GlyphRasterFormat::Alpha8);
    GpuCompositorFrame first_frame;
    GpuFrameReceipt first_receipt;
    assert(prepare_gpu_compositor_frame(
        first.request(), &cache, &backend, &first_frame, nullptr, &error));
    assert(submit_gpu_compositor_frame(
        first_frame, &cache, &backend, &first_receipt, &error));
    cache.retire_completed_frames(first_receipt.fence_value);

    Fixture second = Fixture::one_page(GlyphRasterFormat::LcdRgb8);
    auto second_request = second.request();
    second_request.frame_generation = 42U;
    second_request.completed_upload_fence = first_receipt.fence_value;
    GpuCompositorFrame second_frame;
    GpuFrameReceipt second_receipt;
    assert(prepare_gpu_compositor_frame(
        second_request, &cache, &backend, &second_frame, nullptr, &error));
    assert(submit_gpu_compositor_frame(
        second_frame, &cache, &backend, &second_receipt, &error));
    cache.retire_completed_frames(second_receipt.fence_value);
    assert(cache.snapshot().texture_count == 2U);

    Fixture third = Fixture::one_page(GlyphRasterFormat::Bgra8);
    auto third_request = third.request();
    third_request.frame_generation = 43U;
    third_request.completed_upload_fence = second_receipt.fence_value;
    GpuCompositorFrame third_frame;
    GpuCompositorFrameStats third_stats;
    assert(prepare_gpu_compositor_frame(
        third_request, &cache, &backend, &third_frame, &third_stats, &error));
    assert(third_stats.evicted_textures == 1U);
    const auto snapshot = cache.snapshot();
    assert(snapshot.texture_count == 2U);
    assert(snapshot.evicted_textures == 1U);
    assert(snapshot.released_textures == 1U);
}

void test_limits_and_output_failure_atomicity() {
    Fixture fixture = Fixture::three_pages();
    ReferenceGpuCompositorBackend backend;
    GpuTextureResidencyCache small_cache(
        {256U, 256U, 2U, 1U, 1U},
        64U << 10U);
    GpuCompositorFrame frame;
    GpuCompositorFrameError error;
    assert(!prepare_gpu_compositor_frame(
        fixture.request(), &small_cache, &backend, &frame, nullptr, &error));
    assert(error.kind == GpuCompositorFrameErrorKind::TextureCapacityExceeded);
    assert(frame.commands.empty());

    FailingMemoryResource failing(1U);
    GpuTextureResidencyCache cache(
        {256U, 256U, 3U, 1U, 1U},
        64U << 10U);
    GpuCompositorFrame tiny(&failing);
    assert(!prepare_gpu_compositor_frame(
        fixture.request(), &cache, &backend, &tiny, nullptr, &error));
    assert(error.kind == GpuCompositorFrameErrorKind::OutputBudgetExceeded);
    assert(tiny.commands.empty());
    assert(cache.snapshot().texture_count == 0U);
}

} // namespace

int main() {
    test_cold_frame_and_submission_ring();
    test_hot_frame_reuses_textures();
    test_clear_invalidates_frame();
    test_pending_zero_fence_is_not_promoted();
    test_safe_lru_eviction();
    test_limits_and_output_failure_atomicity();
    std::cout << "gpu-compositor-submission-tests: PASS\n";
    return 0;
}
