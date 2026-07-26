#include "gpu_compositor_submission.hpp"

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
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > hard_limit_ - current_) {
            throw std::bad_alloc();
        }
        void* value = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        current_ += bytes;
        peak_ = std::max(peak_, current_);
        return value;
    }
    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        current_ -= bytes;
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t hard_limit_{0};
    std::size_t current_{0};
    std::size_t peak_{0};
};

void mix(std::uint64_t value, std::uint64_t* hash) noexcept {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        *hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
        *hash *= 1'099'511'628'211ULL;
    }
}

struct Fixture final {
    GlyphRasterWorkingSet working;
    std::array<std::byte, 256> font_bytes{};
    DeviceRasterFaceSource face;
    DeviceGlyphRasterPlan plan;
    DeviceGlyphRasterSourceSet sources;
    GlyphAtlasCache atlas_cache;
    GlyphAtlasSubmission atlas;
    GlyphAtlasUploadExecution uploads;
    TextPaintCommandStream paint;

    Fixture()
        : atlas_cache(
              {256U, 256U, 3U, 128U, 1U, 0U},
              64U << 10U) {
        working.entries.reserve(96U);
        working.uses.reserve(320U);
        std::uint32_t first_use = 0U;
        for (std::uint32_t index = 0U; index < 96U; ++index) {
            GlyphRasterWorkingSetEntry entry;
            entry.key.font_generation_id = 7U;
            entry.key.face_id = 3U;
            entry.key.glyph_id = index + 1U;
            entry.key.x_scale = 1'024;
            entry.key.y_scale = 1'024;
            entry.key.mode = index < 64U
                ? GlyphRasterMode::Grayscale
                : (index < 80U ? GlyphRasterMode::Lcd
                               : GlyphRasterMode::Color);
            entry.key.subpixel_x = entry.key.mode == GlyphRasterMode::Color
                ? 0U
                : static_cast<std::uint8_t>(index % 3U);
            entry.key.subpixel_y = entry.key.subpixel_x;
            entry.first_use_index = first_use;
            entry.use_count = index < 32U ? 4U : 3U;
            working.entries.push_back(entry);
            for (std::uint32_t use_index = 0U;
                 use_index < entry.use_count;
                 ++use_index) {
                GlyphRasterUseRecord use;
                use.viewport_inline_origin =
                    static_cast<std::int64_t>((index % 4U) * 40U + use_index * 8U);
                use.viewport_baseline_origin =
                    static_cast<std::int64_t>((index / 4U) * 20U + 16U);
                use.key_index = index;
                use.paint_command_index = first_use + use_index;
                use.glyph_batch_index = index;
                use.glyph_index = use_index;
                use.style_id = index < 64U ? 1U : (index < 80U ? 2U : 3U);
                use.clip_index = 0U;
                use.source_line_index = 8'184U + index / 4U;
                working.uses.push_back(use);
            }
            first_use += entry.use_count;
        }
        assert(working.uses.size() == 320U);

        for (std::size_t index = 0U; index < font_bytes.size(); ++index) {
            font_bytes[index] = static_cast<std::byte>(index);
        }
        face.font_generation_id = 7U;
        face.face_id = 3U;
        face.resource_id = 99U;
        face.bytes = font_bytes;

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
        assert(sources.sources.size() == 96U);
        assert(sources.payload.size() == 12'720U);

        const GlyphAtlasSubmissionRequest atlas_request{
            &working,
            sources.sources,
            sources.payload,
            {96U, 1U << 20U, 320U, 3U}};
        GlyphAtlasSubmissionError atlas_error;
        assert(prepare_glyph_atlas_submission(
            atlas_request,
            &atlas_cache,
            &atlas,
            nullptr,
            &atlas_error));
        assert(atlas.uploads.size() == 93U);
        assert(atlas.draw_instances.size() == 310U);
        assert(atlas.draw_batches.size() == 3U);

        ReferenceGlyphAtlasUploadBackend upload_backend;
        const GlyphAtlasUploadExecutionRequest upload_request{
            &atlas,
            &atlas_cache,
            sources.payload,
            {3U, 1U << 20U}};
        GlyphAtlasUploadExecutionError upload_error;
        assert(execute_glyph_atlas_uploads(
            upload_request,
            &upload_backend,
            &uploads,
            nullptr,
            &upload_error));
        assert(uploads.batches.size() == 3U);

        paint.clips.push_back({0, 0, 1'024U, 80'000U});
        for (std::uint32_t index = 0U; index < 64U; ++index) {
            paint.fill_rects.push_back({
                static_cast<std::int64_t>((index % 4U) * 256U),
                static_cast<std::int64_t>((index / 4U) * 1'000U),
                256U,
                1'000U,
                101U,
                8'184U + index,
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
        paint.fill_rects.push_back({
            511,
            32'000,
            2U,
            1'000U,
            102U,
            8'216U,
            128U,
            0U});
        paint.commands.push_back({
            TextPaintCommandKind::CaretRect,
            64U,
            0U,
            0U});
    }
};

std::uint64_t checksum_frame(
    const GpuCompositorFrame& cold,
    const GpuFrameReceipt& cold_receipt,
    const GpuCompositorFrame& warm,
    const GpuFrameReceipt& warm_receipt) {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    const auto hash_frame = [&](const GpuCompositorFrame& frame) {
        mix(frame.device_generation, &hash);
        mix(frame.frame_generation, &hash);
        mix(frame.atlas_generation_id, &hash);
        mix(frame.required_upload_fence, &hash);
        for (const GpuTextureUploadCommand& upload : frame.texture_uploads) {
            mix(upload.texture.texture_id, &hash);
            mix(upload.texture.texture_generation, &hash);
            mix(upload.texture.page_index, &hash);
            mix(upload.first_upload, &hash);
            mix(upload.upload_count, &hash);
        }
        for (const GpuGlyphDrawPacket& draw : frame.glyph_draws) {
            mix(draw.texture.texture_id, &hash);
            mix(draw.required_fence_value, &hash);
            mix(draw.first_instance, &hash);
            mix(draw.instance_count, &hash);
            mix(draw.style_id, &hash);
            mix(draw.clip_index, &hash);
            mix(draw.flags, &hash);
        }
        for (const GpuCompositorCommandRecord& command : frame.commands) {
            mix(static_cast<std::uint64_t>(command.kind), &hash);
            mix(command.payload_index, &hash);
            mix(command.clip_index, &hash);
            mix(command.flags, &hash);
        }
    };
    hash_frame(cold);
    mix(cold_receipt.frame_id, &hash);
    mix(cold_receipt.fence_value, &hash);
    hash_frame(warm);
    mix(warm_receipt.frame_id, &hash);
    mix(warm_receipt.fence_value, &hash);
    return hash;
}

struct RunResult final {
    double milliseconds{0.0};
    std::uint64_t checksum{0};
    std::size_t output_current{0};
    std::size_t output_peak{0};
    GpuTextureResidencyStats cache;
    std::uint64_t cold_upload_commands{0};
    std::uint64_t warm_upload_commands{0};
    std::uint64_t glyph_draws{0};
    std::uint64_t fill_rects{0};
    std::uint64_t commands{0};
    std::uint64_t cold_fence{0};
    std::uint64_t warm_fence{0};
};

RunResult run_once() {
    constexpr std::size_t kOutputHardLimit = 4'652U;
    constexpr std::size_t kCacheHardLimit = 624U;
    CountingMemoryResource cold_output_resource(kOutputHardLimit);
    CountingMemoryResource warm_output_resource(kOutputHardLimit);
    Fixture fixture;
    ReferenceGpuCompositorBackend backend;
    GpuTextureResidencyCache cache(
        {256U, 256U, 3U, 2U, 1U},
        kCacheHardLimit);

    GpuCompositorFrameRequest request;
    request.paint_stream = &fixture.paint;
    request.atlas_submission = &fixture.atlas;
    request.upload_execution = &fixture.uploads;
    request.atlas_cache = &fixture.atlas_cache;
    request.raster_payload = fixture.sources.payload;
    request.frame_generation = 101U;
    request.completed_upload_fence = 0U;
    request.limits = {3U, 3U, 65U, 68U};

    GpuCompositorFrame cold(&cold_output_resource);
    GpuCompositorFrameStats cold_stats;
    GpuCompositorFrameError error;
    GpuFrameReceipt cold_receipt;
    const auto start = std::chrono::steady_clock::now();
    assert(prepare_gpu_compositor_frame(
        request, &cache, &backend, &cold, &cold_stats, &error));
    assert(submit_gpu_compositor_frame(
        cold, &cache, &backend, &cold_receipt, &error));
    cache.retire_completed_frames(cold_receipt.fence_value);

    GpuCompositorFrame warm(&warm_output_resource);
    request.frame_generation = 102U;
    request.completed_upload_fence = cold_receipt.fence_value;
    GpuCompositorFrameStats warm_stats;
    GpuFrameReceipt warm_receipt;
    assert(prepare_gpu_compositor_frame(
        request, &cache, &backend, &warm, &warm_stats, &error));
    assert(submit_gpu_compositor_frame(
        warm, &cache, &backend, &warm_receipt, &error));
    const auto stop = std::chrono::steady_clock::now();

    RunResult result;
    result.milliseconds =
        std::chrono::duration<double, std::milli>(stop - start).count();
    result.checksum = checksum_frame(cold, cold_receipt, warm, warm_receipt);
    result.output_current = std::max(
        cold_output_resource.current(),
        warm_output_resource.current());
    result.output_peak = std::max(
        cold_output_resource.peak(),
        warm_output_resource.peak());
    result.cache = cache.snapshot();
    result.cold_upload_commands = cold.texture_uploads.size();
    result.warm_upload_commands = warm.texture_uploads.size();
    result.glyph_draws = cold.glyph_draws.size();
    result.fill_rects = cold.fill_rects.size();
    result.commands = cold.commands.size();
    result.cold_fence = cold_receipt.fence_value;
    result.warm_fence = warm_receipt.fence_value;
    return result;
}

} // namespace

int main() {
    constexpr std::size_t kIterations = 512U;
    std::vector<double> samples;
    samples.reserve(kIterations);
    RunResult reference;
    for (std::size_t index = 0U; index < kIterations; ++index) {
        RunResult current = run_once();
        if (index == 0U) {
            reference = current;
        } else {
            assert(current.checksum == reference.checksum);
            assert(current.output_current == reference.output_current);
            assert(current.output_peak == reference.output_peak);
            assert(current.cache.metadata.current_bytes ==
                reference.cache.metadata.current_bytes);
            assert(current.cache.metadata.peak_bytes ==
                reference.cache.metadata.peak_bytes);
            assert(current.cold_upload_commands == 3U);
            assert(current.warm_upload_commands == 0U);
        }
        samples.push_back(current.milliseconds);
    }
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double probability) {
        const std::size_t index = static_cast<std::size_t>(
            probability * static_cast<double>(samples.size() - 1U));
        return samples[index];
    };
    std::cout << "{\n"
              << "  \"schema\": \"zevryon.gpu-compositor-submission-benchmark.v1\",\n"
              << "  \"input_document_lines\": 16384,\n"
              << "  \"input_projected_lines\": 80,\n"
              << "  \"input_atlas_uploads\": 93,\n"
              << "  \"input_draw_instances\": 310,\n"
              << "  \"input_draw_batches\": 3,\n"
              << "  \"selection_commands\": 64,\n"
              << "  \"caret_commands\": 1,\n"
              << "  \"texture_pages\": 3,\n"
              << "  \"cold_texture_upload_commands\": "
              << reference.cold_upload_commands << ",\n"
              << "  \"warm_texture_upload_commands\": "
              << reference.warm_upload_commands << ",\n"
              << "  \"glyph_draw_packets\": " << reference.glyph_draws << ",\n"
              << "  \"fill_rect_packets\": " << reference.fill_rects << ",\n"
              << "  \"compositor_commands\": " << reference.commands << ",\n"
              << "  \"cold_frame_fence\": " << reference.cold_fence << ",\n"
              << "  \"warm_frame_fence\": " << reference.warm_fence << ",\n"
              << "  \"texture_handle_bytes\": " << sizeof(GpuTextureHandle) << ",\n"
              << "  \"residency_record_bytes\": "
              << sizeof(GpuTextureResidencyRecord) << ",\n"
              << "  \"upload_command_bytes\": "
              << sizeof(GpuTextureUploadCommand) << ",\n"
              << "  \"draw_packet_bytes\": " << sizeof(GpuGlyphDrawPacket) << ",\n"
              << "  \"fill_packet_bytes\": " << sizeof(GpuFillRectPacket) << ",\n"
              << "  \"command_record_bytes\": "
              << sizeof(GpuCompositorCommandRecord) << ",\n"
              << "  \"frame_receipt_bytes\": " << sizeof(GpuFrameReceipt) << ",\n"
              << "  \"output_current_bytes\": " << reference.output_current << ",\n"
              << "  \"output_peak_bytes\": " << reference.output_peak << ",\n"
              << "  \"output_hard_limit_bytes\": 4652,\n"
              << "  \"cache_current_bytes\": "
              << reference.cache.metadata.current_bytes << ",\n"
              << "  \"cache_peak_bytes\": "
              << reference.cache.metadata.peak_bytes << ",\n"
              << "  \"cache_hard_limit_bytes\": 624,\n"
              << "  \"cache_textures\": " << reference.cache.texture_count << ",\n"
              << "  \"cache_in_flight_frames\": "
              << reference.cache.in_flight_count << ",\n"
              << "  \"checksum\": " << reference.checksum << ",\n"
              << "  \"p50_ms\": " << percentile(0.50) << ",\n"
              << "  \"p95_ms\": " << percentile(0.95) << ",\n"
              << "  \"p99_ms\": " << percentile(0.99) << ",\n"
              << "  \"maximum_ms\": " << samples.back() << "\n"
              << "}\n";
    return 0;
}
