#include "gpu_atlas_frame_submission.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <span>
#include <vector>

namespace {
using namespace zevryon::text;

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    explicit CountingMemoryResource(std::size_t hard_limit)
        : hard_limit_(hard_limit) {}

    std::size_t current() const noexcept { return current_; }
    std::size_t peak() const noexcept { return peak_; }
    std::size_t hard_limit() const noexcept { return hard_limit_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > hard_limit_ - current_) {
            throw std::bad_alloc();
        }
        void* pointer =
            std::pmr::new_delete_resource()->allocate(bytes, alignment);
        current_ += bytes;
        peak_ = std::max(peak_, current_);
        return pointer;
    }

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        current_ -= bytes;
        std::pmr::new_delete_resource()->deallocate(
            pointer, bytes, alignment);
    }

    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t hard_limit_;
    std::size_t current_{0};
    std::size_t peak_{0};
};

void hash_u64(std::uint64_t value, std::uint64_t* hash) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        *hash ^= static_cast<std::uint8_t>((value >> shift) & 0xFFU);
        *hash *= 1'099'511'628'211ULL;
    }
}

GlyphRasterWorkingSet build_working_set() {
    GlyphRasterWorkingSet working;
    working.entries.reserve(96U);
    working.uses.reserve(320U);
    std::uint32_t first_use = 0U;
    for (std::uint32_t i = 0U; i < 96U; ++i) {
        GlyphRasterWorkingSetEntry entry;
        entry.key.font_generation_id = 7U;
        entry.key.face_id = 3U;
        entry.key.glyph_id = i + 1U;
        entry.key.x_scale = 1'024;
        entry.key.y_scale = 1'024;
        entry.key.mode = i < 64U ? GlyphRasterMode::Grayscale :
            (i < 80U ? GlyphRasterMode::Lcd : GlyphRasterMode::Color);
        entry.key.subpixel_x = entry.key.mode == GlyphRasterMode::Color ? 0U :
            static_cast<std::uint8_t>(i % 3U);
        entry.key.subpixel_y = entry.key.subpixel_x;
        entry.first_use_index = first_use;
        entry.use_count = i < 32U ? 4U : 3U;
        working.entries.push_back(entry);
        for (std::uint32_t use_index = 0U;
             use_index < entry.use_count;
             ++use_index) {
            GlyphRasterUseRecord use;
            use.viewport_inline_origin =
                static_cast<std::int64_t>((i % 4U) * 40U + use_index * 8U);
            use.viewport_baseline_origin =
                static_cast<std::int64_t>((i / 4U) * 20U + 16U);
            use.key_index = i;
            use.paint_command_index = first_use + use_index;
            use.glyph_batch_index = i;
            use.glyph_index = use_index;
            use.style_id = i < 64U ? 1U : (i < 80U ? 2U : 3U);
            use.clip_index = 0U;
            use.source_line_index = 8'184U + i / 4U;
            working.uses.push_back(use);
        }
        first_use += entry.use_count;
    }
    assert(working.uses.size() == 320U);
    return working;
}

TextPaintCommandStream build_paint_stream() {
    TextPaintCommandStream paint;
    TextPaintClipRect clip;
    clip.inline_size = 1'280U;
    clip.block_size = 1'280U;
    paint.clips.push_back(clip);

    for (std::uint32_t i = 0U; i < 64U; ++i) {
        TextPaintFillRect fill;
        fill.viewport_inline_start = static_cast<std::int64_t>((i % 4U) * 40U);
        fill.viewport_block_start = static_cast<std::int64_t>((i / 4U) * 20U);
        fill.inline_size = 32U;
        fill.block_size = 18U;
        fill.style_id = 10U;
        fill.source_line_index = 8'184U + i / 4U;
        fill.source_fragment_index = i;
        fill.flags = kTextPaintRectSelection;
        paint.fill_rects.push_back(fill);
        paint.commands.push_back({
            TextPaintCommandKind::SelectionRect, i, 0U, 0U});
    }

    for (std::uint32_t i = 0U; i < 80U; ++i) {
        TextPaintGlyphBatch batch;
        batch.glyph_count = 4U;
        batch.style_id = i < 54U ? 1U : (i < 67U ? 2U : 3U);
        batch.source_line_index = 8'184U + i;
        paint.glyph_batches.push_back(batch);
        paint.commands.push_back({
            TextPaintCommandKind::GlyphBatch, i, 0U, 0U});
    }

    TextPaintFillRect caret;
    caret.viewport_inline_start = 640;
    caret.viewport_block_start = 640;
    caret.inline_size = 1U;
    caret.block_size = 20U;
    caret.style_id = 11U;
    caret.source_line_index = 8'248U;
    caret.flags = kTextPaintRectCaret;
    paint.fill_rects.push_back(caret);
    paint.commands.push_back({
        TextPaintCommandKind::CaretRect, 64U, 0U, 0U});
    return paint;
}

std::uint64_t checksum(
    const GpuFrameSubmission& frame,
    const GpuFrameReceipt& cold_receipt,
    const GpuFrameReceipt& hot_receipt,
    const GpuAtlasFrameSchedulerSnapshot& scheduler) {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    hash_u64(frame.commands.size(), &hash);
    hash_u64(frame.fill_rects.size(), &hash);
    hash_u64(frame.glyph_batches.size(), &hash);
    hash_u64(frame.page_references.size(), &hash);
    hash_u64(frame.required_upload_fence, &hash);
    for (const GpuFrameCommandRecord& command : frame.commands) {
        hash_u64(static_cast<std::uint64_t>(command.kind), &hash);
        hash_u64(command.payload_index, &hash);
        hash_u64(command.clip_index, &hash);
        hash_u64(command.flags, &hash);
    }
    for (const GpuFrameGlyphBatch& batch : frame.glyph_batches) {
        hash_u64(batch.page_generation, &hash);
        hash_u64(batch.page_index, &hash);
        hash_u64(batch.first_instance, &hash);
        hash_u64(batch.instance_count, &hash);
        hash_u64(batch.style_id, &hash);
        hash_u64(batch.clip_index, &hash);
        hash_u64(batch.page_reference_index, &hash);
    }
    for (const GpuFramePageReference& page : frame.page_references) {
        hash_u64(page.page_generation, &hash);
        hash_u64(page.required_upload_fence, &hash);
        hash_u64(page.page_index, &hash);
        hash_u64(page.batch_count, &hash);
        hash_u64(static_cast<std::uint64_t>(page.format), &hash);
    }
    hash_u64(cold_receipt.submit_fence_value, &hash);
    hash_u64(hot_receipt.submit_fence_value, &hash);
    hash_u64(scheduler.scheduler_generation, &hash);
    hash_u64(scheduler.atlas_generation_id, &hash);
    hash_u64(scheduler.submitted_frames, &hash);
    hash_u64(scheduler.retired_frames, &hash);
    hash_u64(scheduler.resident_page_count, &hash);
    hash_u64(scheduler.in_flight_frame_count, &hash);
    hash_u64(scheduler.page_pin_count, &hash);
    return hash;
}

struct RunResult final {
    double milliseconds{0.0};
    std::uint64_t frame_commands{0};
    std::uint64_t fill_rects{0};
    std::uint64_t glyph_batches{0};
    std::uint64_t page_references{0};
    std::uint64_t cold_wait_fence{0};
    std::uint64_t hot_wait_fence{0};
    std::uint64_t cold_signal_fence{0};
    std::uint64_t hot_signal_fence{0};
    std::uint64_t submitted_frames{0};
    std::uint64_t retired_frames{0};
    std::uint64_t resident_pages{0};
    std::uint64_t checksum{0};
    std::size_t output_current{0};
    std::size_t output_peak{0};
    GpuAtlasFrameSchedulerSnapshot scheduler;
};

RunResult run_once() {
    GlyphRasterWorkingSet working = build_working_set();
    TextPaintCommandStream paint = build_paint_stream();
    std::array<std::byte, 256> font_bytes{};
    for (std::size_t i = 0; i < font_bytes.size(); ++i) {
        font_bytes[i] = static_cast<std::byte>(i);
    }
    DeviceRasterFaceSource face;
    face.font_generation_id = 7U;
    face.face_id = 3U;
    face.resource_id = 99U;
    face.bytes = font_bytes;

    DeviceGlyphRasterPlan plan;
    DeviceGlyphRasterPlanRequest plan_request;
    plan_request.working_set = &working;
    plan_request.face_sources =
        std::span<const DeviceRasterFaceSource>(&face, 1U);
    plan_request.queue_generation = 41U;
    plan_request.atlas_generation_id = 1U;
    plan_request.limits.maximum_jobs = 96U;
    DeviceGlyphRasterPlanError plan_error;
    assert(build_device_glyph_raster_plan(
        plan_request, &plan, nullptr, &plan_error));

    ReferenceDeviceGlyphRasterBackend raster_backend;
    DeviceGlyphRasterSourceSet sources;
    DeviceGlyphRasterExecutionRequest raster_request;
    raster_request.plan = &plan;
    raster_request.face_sources =
        std::span<const DeviceRasterFaceSource>(&face, 1U);
    raster_request.expected_queue_generation = 41U;
    raster_request.limits.maximum_sources = 96U;
    raster_request.limits.maximum_payload_bytes = 1U << 20U;
    DeviceGlyphRasterExecutionError raster_error;
    assert(execute_device_glyph_raster_plan(
        raster_request,
        &raster_backend,
        &sources,
        nullptr,
        &raster_error));

    GlyphAtlasCache cache(
        GlyphAtlasConfig{256U, 256U, 3U, 128U, 1U, 0U},
        64U << 10U);
    GlyphAtlasSubmission cold;
    GlyphAtlasSubmissionRequest cold_request;
    cold_request.working_set = &working;
    cold_request.raster_sources = sources.sources;
    cold_request.raster_payload = sources.payload;
    cold_request.limits.maximum_uploads = 96U;
    cold_request.limits.maximum_upload_bytes = 1U << 20U;
    cold_request.limits.maximum_draw_instances = 320U;
    cold_request.limits.maximum_draw_batches = 320U;
    GlyphAtlasSubmissionError submission_error;
    assert(prepare_glyph_atlas_submission(
        cold_request,
        &cache,
        &cold,
        nullptr,
        &submission_error));

    GlyphAtlasUploadExecution cold_upload;
    GlyphAtlasUploadExecutionRequest upload_request;
    upload_request.submission = &cold;
    upload_request.cache = &cache;
    upload_request.raster_payload = sources.payload;
    upload_request.limits.maximum_batches = 96U;
    upload_request.limits.maximum_upload_bytes = 1U << 20U;
    ReferenceGlyphAtlasUploadBackend upload_backend;
    GlyphAtlasUploadExecutionError upload_error;
    assert(execute_glyph_atlas_uploads(
        upload_request,
        &upload_backend,
        &cold_upload,
        nullptr,
        &upload_error));

    GlyphAtlasSubmission hot;
    GlyphAtlasSubmissionRequest hot_request;
    hot_request.working_set = &working;
    hot_request.limits = cold_request.limits;
    assert(prepare_glyph_atlas_submission(
        hot_request,
        &cache,
        &hot,
        nullptr,
        &submission_error));
    assert(hot.uploads.empty());

    GlyphAtlasUploadExecution hot_upload;
    upload_request.submission = &hot;
    upload_request.raster_payload = {};
    assert(execute_glyph_atlas_uploads(
        upload_request,
        &upload_backend,
        &hot_upload,
        nullptr,
        &upload_error));
    assert(hot_upload.receipts.empty());

    constexpr std::size_t kOutputHardLimit = 64U << 10U;
    CountingMemoryResource output_resource(kOutputHardLimit);
    GpuFrameSubmission frame(&output_resource);
    GpuFrameSubmissionRequest frame_request;
    frame_request.paint_stream = &paint;
    frame_request.working_set = &working;
    frame_request.cache = &cache;
    frame_request.surface.surface_id = 7U;
    frame_request.surface.generation_id = 1U;
    frame_request.surface.width = 1'280U;
    frame_request.surface.height = 1'280U;
    frame_request.limits.maximum_clips = 1U;
    frame_request.limits.maximum_commands = 128U;
    frame_request.limits.maximum_fill_rects = 65U;
    frame_request.limits.maximum_glyph_batches = 8U;
    frame_request.limits.maximum_page_references = 3U;
    frame_request.limits.maximum_referenced_instances = 320U;

    GpuAtlasFrameScheduler scheduler(
        GpuAtlasFrameSchedulerConfig{3U, 2U, 6U, 0U},
        64U << 10U);
    ReferenceGpuFrameBackend frame_backend;
    GpuFrameReceipt cold_receipt;
    GpuFrameReceipt hot_receipt;
    GpuFrameSubmissionError frame_error;
    GpuFrameSubmitError submit_error;
    GpuFrameRetireError retire_error;

    const auto start = std::chrono::steady_clock::now();
    frame_request.atlas_submission = &cold;
    frame_request.upload_execution = &cold_upload;
    frame_request.frame_id = 1U;
    assert(prepare_gpu_frame_submission(
        frame_request, &frame, nullptr, &frame_error));
    const std::uint64_t cold_wait = frame.required_upload_fence;

    GpuFrameSubmitRequest submit_request;
    submit_request.frame = &frame;
    submit_request.atlas_submission = &cold;
    submit_request.upload_execution = &cold_upload;
    submit_request.cache = &cache;
    assert(submit_gpu_frame(
        submit_request,
        &frame_backend,
        &scheduler,
        &cold_receipt,
        nullptr,
        &submit_error));
    assert(retire_gpu_frames(
        &scheduler,
        cold_receipt.submit_fence_value,
        nullptr,
        &retire_error));

    frame_request.atlas_submission = &hot;
    frame_request.upload_execution = &hot_upload;
    frame_request.frame_id = 2U;
    assert(prepare_gpu_frame_submission(
        frame_request, &frame, nullptr, &frame_error));
    const std::uint64_t hot_wait = frame.required_upload_fence;
    submit_request.frame = &frame;
    submit_request.atlas_submission = &hot;
    submit_request.upload_execution = &hot_upload;
    assert(submit_gpu_frame(
        submit_request,
        &frame_backend,
        &scheduler,
        &hot_receipt,
        nullptr,
        &submit_error));
    assert(retire_gpu_frames(
        &scheduler,
        hot_receipt.submit_fence_value,
        nullptr,
        &retire_error));
    const auto stop = std::chrono::steady_clock::now();

    RunResult result;
    result.milliseconds =
        std::chrono::duration<double, std::milli>(stop - start).count();
    result.frame_commands = frame.commands.size();
    result.fill_rects = frame.fill_rects.size();
    result.glyph_batches = frame.glyph_batches.size();
    result.page_references = frame.page_references.size();
    result.cold_wait_fence = cold_wait;
    result.hot_wait_fence = hot_wait;
    result.cold_signal_fence = cold_receipt.submit_fence_value;
    result.hot_signal_fence = hot_receipt.submit_fence_value;
    result.output_current = output_resource.current();
    result.output_peak = output_resource.peak();
    result.scheduler = scheduler.snapshot();
    result.submitted_frames = result.scheduler.submitted_frames;
    result.retired_frames = result.scheduler.retired_frames;
    result.resident_pages = result.scheduler.resident_page_count;
    result.checksum = checksum(
        frame, cold_receipt, hot_receipt, result.scheduler);
    return result;
}

} // namespace

int main() {
    constexpr std::size_t kIterations = 256U;
    std::vector<double> samples;
    samples.reserve(kIterations);
    RunResult reference;
    for (std::size_t i = 0; i < kIterations; ++i) {
        RunResult current = run_once();
        if (i == 0U) {
            reference = current;
        } else {
            assert(current.frame_commands == reference.frame_commands);
            assert(current.fill_rects == reference.fill_rects);
            assert(current.glyph_batches == reference.glyph_batches);
            assert(current.page_references == reference.page_references);
            assert(current.cold_wait_fence == reference.cold_wait_fence);
            assert(current.hot_wait_fence == 0U);
            assert(current.cold_signal_fence == reference.cold_signal_fence);
            assert(current.hot_signal_fence == reference.hot_signal_fence);
            assert(current.submitted_frames == 2U);
            assert(current.retired_frames == 2U);
            assert(current.resident_pages == 3U);
            assert(current.scheduler.in_flight_frame_count == 0U);
            assert(current.scheduler.page_pin_count == 0U);
            assert(current.output_current == reference.output_current);
            assert(current.output_peak == reference.output_peak);
            assert(current.scheduler.metadata.current_bytes ==
                reference.scheduler.metadata.current_bytes);
            assert(current.scheduler.metadata.peak_bytes ==
                reference.scheduler.metadata.peak_bytes);
            assert(current.checksum == reference.checksum);
        }
        samples.push_back(current.milliseconds);
    }
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double p) {
        const std::size_t index = static_cast<std::size_t>(
            p * static_cast<double>(samples.size() - 1U));
        return samples[index];
    };
    std::cout << "{\n"
              << "  \"schema\": \"zevryon.gpu-atlas-frame-submission-benchmark.v1\",\n"
              << "  \"input_document_lines\": 16384,\n"
              << "  \"input_projected_lines\": 80,\n"
              << "  \"input_paint_commands\": 145,\n"
              << "  \"input_draw_instances\": 310,\n"
              << "  \"frame_commands\": " << reference.frame_commands << ",\n"
              << "  \"fill_rects\": " << reference.fill_rects << ",\n"
              << "  \"glyph_batches\": " << reference.glyph_batches << ",\n"
              << "  \"page_references\": " << reference.page_references << ",\n"
              << "  \"cold_wait_fence\": " << reference.cold_wait_fence << ",\n"
              << "  \"hot_wait_fence\": " << reference.hot_wait_fence << ",\n"
              << "  \"cold_signal_fence\": " << reference.cold_signal_fence << ",\n"
              << "  \"hot_signal_fence\": " << reference.hot_signal_fence << ",\n"
              << "  \"submitted_frames\": " << reference.submitted_frames << ",\n"
              << "  \"retired_frames\": " << reference.retired_frames << ",\n"
              << "  \"resident_pages\": " << reference.resident_pages << ",\n"
              << "  \"in_flight_frames\": "
              << reference.scheduler.in_flight_frame_count << ",\n"
              << "  \"page_pins\": " << reference.scheduler.page_pin_count << ",\n"
              << "  \"output_current_bytes\": " << reference.output_current << ",\n"
              << "  \"output_peak_bytes\": " << reference.output_peak << ",\n"
              << "  \"scheduler_current_bytes\": "
              << reference.scheduler.metadata.current_bytes << ",\n"
              << "  \"scheduler_peak_bytes\": "
              << reference.scheduler.metadata.peak_bytes << ",\n"
              << "  \"checksum\": " << reference.checksum << ",\n"
              << "  \"p50_ms\": " << percentile(0.50) << ",\n"
              << "  \"p95_ms\": " << percentile(0.95) << ",\n"
              << "  \"p99_ms\": " << percentile(0.99) << ",\n"
              << "  \"maximum_ms\": " << samples.back() << "\n"
              << "}\n";
    return 0;
}
