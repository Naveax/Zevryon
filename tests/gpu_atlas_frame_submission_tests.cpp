#include "gpu_atlas_frame_submission.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <new>
#include <span>

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
        void* pointer =
            std::pmr::new_delete_resource()->allocate(bytes, alignment);
        used_ += bytes;
        return pointer;
    }

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        used_ -= bytes;
        std::pmr::new_delete_resource()->deallocate(
            pointer, bytes, alignment);
    }

    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t limit_;
    std::size_t used_{0};
};

GlyphRasterKey make_key(
    std::uint32_t glyph_id,
    GlyphRasterMode mode,
    std::uint8_t phase = 0U) {
    GlyphRasterKey key;
    key.font_generation_id = 7U;
    key.face_id = 3U;
    key.glyph_id = glyph_id;
    key.x_scale = 1'024;
    key.y_scale = 1'024;
    key.mode = mode;
    key.subpixel_x = phase;
    key.subpixel_y = phase;
    return key;
}

GlyphRasterWorkingSet make_working_set() {
    GlyphRasterWorkingSet working;
    const std::array<GlyphRasterKey, 4> keys{
        make_key(1U, GlyphRasterMode::Grayscale),
        make_key(2U, GlyphRasterMode::Lcd),
        make_key(3U, GlyphRasterMode::Color),
        make_key(29U, GlyphRasterMode::Grayscale)};
    for (std::uint32_t i = 0U; i < keys.size(); ++i) {
        GlyphRasterWorkingSetEntry entry;
        entry.key = keys[i];
        entry.first_use_index = i;
        entry.use_count = 1U;
        working.entries.push_back(entry);

        GlyphRasterUseRecord use;
        use.viewport_inline_origin = static_cast<std::int64_t>(i * 20U);
        use.viewport_baseline_origin = 50;
        use.key_index = i;
        use.style_id = i < 2U ? 1U : 2U;
        use.clip_index = 0U;
        working.uses.push_back(use);
    }
    return working;
}

TextPaintCommandStream make_paint_stream() {
    TextPaintCommandStream paint;
    TextPaintClipRect clip;
    clip.inline_size = 640U;
    clip.block_size = 480U;
    paint.clips.push_back(clip);

    TextPaintFillRect selection;
    selection.viewport_inline_start = 4;
    selection.viewport_block_start = 8;
    selection.inline_size = 80U;
    selection.block_size = 20U;
    selection.style_id = 10U;
    selection.flags = kTextPaintRectSelection;
    paint.fill_rects.push_back(selection);

    TextPaintFillRect caret;
    caret.viewport_inline_start = 90;
    caret.viewport_block_start = 8;
    caret.inline_size = 1U;
    caret.block_size = 20U;
    caret.style_id = 11U;
    caret.flags = kTextPaintRectCaret;
    paint.fill_rects.push_back(caret);

    TextPaintGlyphBatch glyphs;
    glyphs.glyph_count = 4U;
    glyphs.style_id = 1U;
    paint.glyph_batches.push_back(glyphs);

    paint.commands.push_back({TextPaintCommandKind::SelectionRect, 0U, 0U, 0U});
    paint.commands.push_back({TextPaintCommandKind::GlyphBatch, 0U, 0U, 0U});
    paint.commands.push_back({TextPaintCommandKind::CaretRect, 1U, 0U, 0U});
    return paint;
}

struct ColdPipeline final {
    std::array<std::byte, 64> face_bytes{};
    DeviceRasterFaceSource face;
    GlyphRasterWorkingSet working;
    DeviceGlyphRasterPlan plan;
    DeviceGlyphRasterSourceSet sources;
    GlyphAtlasCache cache;
    GlyphAtlasSubmission cold_submission;
    GlyphAtlasUploadExecution cold_upload;
    TextPaintCommandStream paint;

    ColdPipeline()
        : working(make_working_set()),
          cache(GlyphAtlasConfig{128U, 128U, 3U, 16U, 1U, 0U}, 1U << 20U),
          paint(make_paint_stream()) {
        face.font_generation_id = 7U;
        face.face_id = 3U;
        face.resource_id = 99U;
        face.bytes = face_bytes;

        DeviceGlyphRasterPlanRequest plan_request;
        plan_request.working_set = &working;
        plan_request.face_sources =
            std::span<const DeviceRasterFaceSource>(&face, 1U);
        plan_request.queue_generation = 3U;
        plan_request.atlas_generation_id = 1U;
        plan_request.limits.maximum_jobs = 8U;
        DeviceGlyphRasterPlanError plan_error;
        assert(build_device_glyph_raster_plan(
            plan_request, &plan, nullptr, &plan_error));

        ReferenceDeviceGlyphRasterBackend raster_backend;
        DeviceGlyphRasterExecutionRequest raster_request;
        raster_request.plan = &plan;
        raster_request.face_sources =
            std::span<const DeviceRasterFaceSource>(&face, 1U);
        raster_request.expected_queue_generation = 3U;
        raster_request.limits.maximum_sources = 8U;
        raster_request.limits.maximum_payload_bytes = 1U << 20U;
        DeviceGlyphRasterExecutionError raster_error;
        assert(execute_device_glyph_raster_plan(
            raster_request,
            &raster_backend,
            &sources,
            nullptr,
            &raster_error));

        GlyphAtlasSubmissionRequest submission_request;
        submission_request.working_set = &working;
        submission_request.raster_sources = sources.sources;
        submission_request.raster_payload = sources.payload;
        submission_request.limits.maximum_uploads = 8U;
        submission_request.limits.maximum_upload_bytes = 1U << 20U;
        submission_request.limits.maximum_draw_instances = 8U;
        submission_request.limits.maximum_draw_batches = 8U;
        GlyphAtlasSubmissionError submission_error;
        assert(prepare_glyph_atlas_submission(
            submission_request,
            &cache,
            &cold_submission,
            nullptr,
            &submission_error));

        GlyphAtlasUploadExecutionRequest upload_request;
        upload_request.submission = &cold_submission;
        upload_request.cache = &cache;
        upload_request.raster_payload = sources.payload;
        upload_request.limits.maximum_batches = 8U;
        upload_request.limits.maximum_upload_bytes = 1U << 20U;
        ReferenceGlyphAtlasUploadBackend upload_backend;
        GlyphAtlasUploadExecutionError upload_error;
        assert(execute_glyph_atlas_uploads(
            upload_request,
            &upload_backend,
            &cold_upload,
            nullptr,
            &upload_error));
    }
};

GpuFrameSubmissionRequest make_frame_request(
    const ColdPipeline& pipeline,
    const GlyphAtlasSubmission& submission,
    const GlyphAtlasUploadExecution& upload,
    std::uint64_t frame_id) {
    GpuFrameSubmissionRequest request;
    request.paint_stream = &pipeline.paint;
    request.working_set = &pipeline.working;
    request.atlas_submission = &submission;
    request.upload_execution = &upload;
    request.cache = &pipeline.cache;
    request.surface.surface_id = 5U;
    request.surface.generation_id = 2U;
    request.surface.width = 640U;
    request.surface.height = 480U;
    request.frame_id = frame_id;
    request.limits.maximum_clips = 2U;
    request.limits.maximum_commands = 16U;
    request.limits.maximum_fill_rects = 8U;
    request.limits.maximum_glyph_batches = 8U;
    request.limits.maximum_page_references = 8U;
    request.limits.maximum_referenced_instances = 16U;
    return request;
}

void build_hot_submission(
    ColdPipeline* pipeline,
    GlyphAtlasSubmission* submission,
    GlyphAtlasUploadExecution* upload) {
    GlyphAtlasSubmissionRequest request;
    request.working_set = &pipeline->working;
    request.limits.maximum_uploads = 8U;
    request.limits.maximum_upload_bytes = 1U << 20U;
    request.limits.maximum_draw_instances = 8U;
    request.limits.maximum_draw_batches = 8U;
    GlyphAtlasSubmissionError submission_error;
    assert(prepare_glyph_atlas_submission(
        request,
        &pipeline->cache,
        submission,
        nullptr,
        &submission_error));
    assert(submission->uploads.empty());

    GlyphAtlasUploadExecutionRequest upload_request;
    upload_request.submission = submission;
    upload_request.cache = &pipeline->cache;
    upload_request.limits.maximum_batches = 8U;
    upload_request.limits.maximum_upload_bytes = 1U << 20U;
    ReferenceGlyphAtlasUploadBackend backend;
    GlyphAtlasUploadExecutionError upload_error;
    assert(execute_glyph_atlas_uploads(
        upload_request,
        &backend,
        upload,
        nullptr,
        &upload_error));
    assert(upload->receipts.empty());
}

void test_cold_hot_submit_and_retire() {
    ColdPipeline pipeline;
    GpuFrameSubmission cold_frame;
    GpuFrameSubmissionStats frame_stats;
    GpuFrameSubmissionError frame_error;
    const GpuFrameSubmissionRequest cold_request = make_frame_request(
        pipeline,
        pipeline.cold_submission,
        pipeline.cold_upload,
        1U);
    assert(prepare_gpu_frame_submission(
        cold_request,
        &cold_frame,
        &frame_stats,
        &frame_error));
    assert(cold_frame.commands.size() == 5U);
    assert(cold_frame.fill_rects.size() == 2U);
    assert(cold_frame.glyph_batches.size() == 3U);
    assert(cold_frame.page_references.size() == 3U);
    assert(cold_frame.required_upload_fence ==
        pipeline.cold_upload.last_fence_value);
    assert(gpu_frame_submission_is_current(
        pipeline.cache,
        pipeline.cold_submission,
        pipeline.cold_upload,
        cold_frame));

    GpuAtlasFrameScheduler scheduler(
        GpuAtlasFrameSchedulerConfig{3U, 2U, 6U, 0U},
        1U << 20U);
    ReferenceGpuFrameBackend backend;
    GpuFrameSubmitRequest submit_request;
    submit_request.frame = &cold_frame;
    submit_request.atlas_submission = &pipeline.cold_submission;
    submit_request.upload_execution = &pipeline.cold_upload;
    submit_request.cache = &pipeline.cache;
    GpuFrameReceipt cold_receipt;
    GpuFrameSubmitStats submit_stats;
    GpuFrameSubmitError submit_error;
    assert(submit_gpu_frame(
        submit_request,
        &backend,
        &scheduler,
        &cold_receipt,
        &submit_stats,
        &submit_error));
    assert(gpu_frame_receipt_is_current(scheduler, cold_receipt));
    auto snapshot = scheduler.snapshot();
    assert(snapshot.resident_page_count == 3U);
    assert(snapshot.in_flight_frame_count == 1U);
    assert(snapshot.page_pin_count == 3U);

    GlyphAtlasSubmission hot_submission;
    GlyphAtlasUploadExecution hot_upload;
    build_hot_submission(&pipeline, &hot_submission, &hot_upload);
    GpuFrameSubmission hot_frame;
    const GpuFrameSubmissionRequest hot_request = make_frame_request(
        pipeline, hot_submission, hot_upload, 2U);
    assert(prepare_gpu_frame_submission(
        hot_request, &hot_frame, nullptr, &frame_error));
    assert(hot_frame.required_upload_fence == 0U);

    submit_request.frame = &hot_frame;
    submit_request.atlas_submission = &hot_submission;
    submit_request.upload_execution = &hot_upload;
    GpuFrameReceipt hot_receipt;
    assert(submit_gpu_frame(
        submit_request,
        &backend,
        &scheduler,
        &hot_receipt,
        &submit_stats,
        &submit_error));
    assert(submit_stats.reused_resident_pages == 3U);
    snapshot = scheduler.snapshot();
    assert(snapshot.in_flight_frame_count == 2U);
    assert(snapshot.page_pin_count == 6U);

    GpuFrameRetireStats retire_stats;
    GpuFrameRetireError retire_error;
    assert(retire_gpu_frames(
        &scheduler,
        cold_receipt.submit_fence_value,
        &retire_stats,
        &retire_error));
    assert(retire_stats.retired_frames == 1U);
    assert(retire_stats.released_page_pins == 3U);
    assert(!gpu_frame_receipt_is_current(scheduler, cold_receipt));
    assert(gpu_frame_receipt_is_current(scheduler, hot_receipt));

    assert(retire_gpu_frames(
        &scheduler,
        hot_receipt.submit_fence_value,
        &retire_stats,
        &retire_error));
    assert(retire_stats.remaining_frames == 0U);
    assert(retire_stats.remaining_page_pins == 0U);
    assert(scheduler.clear());
    assert(!gpu_frame_receipt_is_current(scheduler, hot_receipt));
}

void test_stale_and_budget_failures_are_atomic() {
    ColdPipeline pipeline;
    GpuFrameSubmission frame;
    GpuFrameSubmissionError frame_error;
    const GpuFrameSubmissionRequest request = make_frame_request(
        pipeline,
        pipeline.cold_submission,
        pipeline.cold_upload,
        1U);
    assert(prepare_gpu_frame_submission(
        request, &frame, nullptr, &frame_error));

    GpuAtlasFrameScheduler tiny_scheduler(
        GpuAtlasFrameSchedulerConfig{3U, 1U, 3U, 0U}, 1U);
    ReferenceGpuFrameBackend backend;
    GpuFrameSubmitRequest submit_request;
    submit_request.frame = &frame;
    submit_request.atlas_submission = &pipeline.cold_submission;
    submit_request.upload_execution = &pipeline.cold_upload;
    submit_request.cache = &pipeline.cache;
    GpuFrameReceipt receipt;
    GpuFrameSubmitError submit_error;
    assert(!submit_gpu_frame(
        submit_request,
        &backend,
        &tiny_scheduler,
        &receipt,
        nullptr,
        &submit_error));
    assert(submit_error.kind ==
        GpuFrameSubmitErrorKind::MetadataBudgetExceeded);
    assert(receipt.frame_id == 0U);
    assert(tiny_scheduler.snapshot().in_flight_frame_count == 0U);

    FailingMemoryResource failing(1U);
    GpuFrameSubmission bounded(&failing);
    assert(!prepare_gpu_frame_submission(
        request, &bounded, nullptr, &frame_error));
    assert(frame_error.kind ==
        GpuFrameSubmissionErrorKind::OutputBudgetExceeded);
    assert(bounded.commands.empty());
    assert(bounded.fill_rects.empty());
    assert(bounded.glyph_batches.empty());
    assert(bounded.page_references.empty());

    pipeline.cache.clear();
    assert(!gpu_frame_submission_is_current(
        pipeline.cache,
        pipeline.cold_submission,
        pipeline.cold_upload,
        frame));
    GpuAtlasFrameScheduler scheduler(
        GpuAtlasFrameSchedulerConfig{3U, 1U, 3U, 0U},
        1U << 20U);
    assert(!submit_gpu_frame(
        submit_request,
        &backend,
        &scheduler,
        &receipt,
        nullptr,
        &submit_error));
    assert(submit_error.kind == GpuFrameSubmitErrorKind::StaleFrame);
}

void test_invalid_paint_and_retire_regression() {
    ColdPipeline pipeline;
    pipeline.paint.commands = {
        {TextPaintCommandKind::CaretRect, 1U, 0U, 0U},
        {TextPaintCommandKind::SelectionRect, 0U, 0U, 0U}};
    GpuFrameSubmission frame;
    GpuFrameSubmissionError frame_error;
    const GpuFrameSubmissionRequest request = make_frame_request(
        pipeline,
        pipeline.cold_submission,
        pipeline.cold_upload,
        1U);
    assert(!prepare_gpu_frame_submission(
        request, &frame, nullptr, &frame_error));
    assert(frame_error.kind ==
        GpuFrameSubmissionErrorKind::PaintTopologyViolation);

    GpuAtlasFrameScheduler scheduler(
        GpuAtlasFrameSchedulerConfig{1U, 1U, 1U, 0U},
        1U << 20U);
    GpuFrameRetireStats stats;
    GpuFrameRetireError error;
    assert(retire_gpu_frames(&scheduler, 5U, &stats, &error));
    assert(!retire_gpu_frames(&scheduler, 4U, &stats, &error));
    assert(error.kind == GpuFrameRetireErrorKind::FenceRegression);
}

} // namespace

int main() {
    test_cold_hot_submit_and_retire();
    test_stale_and_budget_failures_are_atomic();
    test_invalid_paint_and_retire_regression();
    std::cout << "gpu-atlas-frame-submission-tests: PASS\n";
    return 0;
}
