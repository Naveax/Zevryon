#include "gpu_atlas_frame_submission.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {
using namespace zevryon::text;

GlyphRasterKey make_key(
    std::uint32_t glyph_id,
    GlyphRasterMode mode,
    std::uint8_t phase = 0U) {
    GlyphRasterKey key;
    key.font_generation_id = 17U;
    key.face_id = 5U;
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
        make_key(11U, GlyphRasterMode::Grayscale),
        make_key(12U, GlyphRasterMode::Lcd),
        make_key(13U, GlyphRasterMode::Color),
        make_key(29U, GlyphRasterMode::Grayscale)};
    for (std::uint32_t i = 0U; i < keys.size(); ++i) {
        GlyphRasterWorkingSetEntry entry;
        entry.key = keys[i];
        entry.first_use_index = i;
        entry.use_count = 1U;
        working.entries.push_back(entry);

        GlyphRasterUseRecord use;
        use.viewport_inline_origin = static_cast<std::int64_t>(i * 24U);
        use.viewport_baseline_origin = 64;
        use.key_index = i;
        use.style_id = i + 1U;
        use.clip_index = 0U;
        working.uses.push_back(use);
    }
    return working;
}

struct Pipeline final {
    std::array<std::byte, 128> face_bytes{};
    DeviceRasterFaceSource face;
    GlyphRasterWorkingSet working;
    DeviceGlyphRasterPlan plan;
    DeviceGlyphRasterSourceSet sources;
    GlyphAtlasCache cache;
    GlyphAtlasSubmission cold_submission;
    GlyphAtlasUploadExecution cold_upload;
    GlyphAtlasSubmission hot_submission;
    GlyphAtlasUploadExecution hot_upload;

    Pipeline()
        : working(make_working_set()),
          cache(GlyphAtlasConfig{128U, 128U, 3U, 16U, 1U, 0U}, 1U << 20U) {
        face.font_generation_id = 17U;
        face.face_id = 5U;
        face.resource_id = 501U;
        face.bytes = face_bytes;

        DeviceGlyphRasterPlanRequest plan_request;
        plan_request.working_set = &working;
        plan_request.face_sources =
            std::span<const DeviceRasterFaceSource>(&face, 1U);
        plan_request.queue_generation = 9U;
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
        raster_request.expected_queue_generation = 9U;
        raster_request.limits.maximum_sources = 8U;
        raster_request.limits.maximum_payload_bytes = 1U << 20U;
        DeviceGlyphRasterExecutionError raster_error;
        assert(execute_device_glyph_raster_plan(
            raster_request,
            &raster_backend,
            &sources,
            nullptr,
            &raster_error));

        GlyphAtlasSubmissionRequest cold_request;
        cold_request.working_set = &working;
        cold_request.raster_sources = sources.sources;
        cold_request.raster_payload = sources.payload;
        cold_request.limits.maximum_uploads = 8U;
        cold_request.limits.maximum_upload_bytes = 1U << 20U;
        cold_request.limits.maximum_draw_instances = 8U;
        cold_request.limits.maximum_draw_batches = 8U;
        GlyphAtlasSubmissionError submission_error;
        assert(prepare_glyph_atlas_submission(
            cold_request,
            &cache,
            &cold_submission,
            nullptr,
            &submission_error));

        GlyphAtlasUploadExecutionRequest cold_upload_request;
        cold_upload_request.submission = &cold_submission;
        cold_upload_request.cache = &cache;
        cold_upload_request.raster_payload = sources.payload;
        cold_upload_request.limits.maximum_batches = 8U;
        cold_upload_request.limits.maximum_upload_bytes = 1U << 20U;
        ReferenceGlyphAtlasUploadBackend upload_backend;
        GlyphAtlasUploadExecutionError upload_error;
        assert(execute_glyph_atlas_uploads(
            cold_upload_request,
            &upload_backend,
            &cold_upload,
            nullptr,
            &upload_error));

        GlyphAtlasSubmissionRequest hot_request;
        hot_request.working_set = &working;
        hot_request.limits = cold_request.limits;
        assert(prepare_glyph_atlas_submission(
            hot_request,
            &cache,
            &hot_submission,
            nullptr,
            &submission_error));
        assert(hot_submission.uploads.empty());

        GlyphAtlasUploadExecutionRequest hot_upload_request;
        hot_upload_request.submission = &hot_submission;
        hot_upload_request.cache = &cache;
        hot_upload_request.limits = cold_upload_request.limits;
        assert(execute_glyph_atlas_uploads(
            hot_upload_request,
            &upload_backend,
            &hot_upload,
            nullptr,
            &upload_error));
        assert(hot_upload.receipts.empty());
    }
};

TextPaintCommandStream make_paint_stream(
    std::uint32_t selection_count,
    std::uint32_t glyph_command_count,
    std::uint32_t caret_count,
    std::uint32_t clip_count) {
    TextPaintCommandStream paint;
    for (std::uint32_t i = 0U; i < clip_count; ++i) {
        TextPaintClipRect clip;
        clip.viewport_inline_start = static_cast<std::int64_t>(i * 4U);
        clip.viewport_block_start = static_cast<std::int64_t>(i * 3U);
        clip.inline_size = 640U - i;
        clip.block_size = 480U - i;
        paint.clips.push_back(clip);
    }
    for (std::uint32_t i = 0U; i < selection_count; ++i) {
        TextPaintFillRect fill;
        fill.viewport_inline_start = static_cast<std::int64_t>(10U + i);
        fill.viewport_block_start = 20;
        fill.inline_size = 30U + i;
        fill.block_size = 12U;
        fill.style_id = 100U + i;
        fill.flags = kTextPaintRectSelection;
        paint.fill_rects.push_back(fill);
        paint.commands.push_back({
            TextPaintCommandKind::SelectionRect,
            i,
            i % clip_count,
            0U});
    }
    for (std::uint32_t i = 0U; i < glyph_command_count; ++i) {
        TextPaintGlyphBatch batch;
        batch.glyph_count = 1U;
        batch.style_id = 200U + i;
        paint.glyph_batches.push_back(batch);
        paint.commands.push_back({
            TextPaintCommandKind::GlyphBatch,
            i,
            i % clip_count,
            0U});
    }
    for (std::uint32_t i = 0U; i < caret_count; ++i) {
        TextPaintFillRect fill;
        fill.viewport_inline_start = static_cast<std::int64_t>(90U + i);
        fill.viewport_block_start = 20;
        fill.inline_size = 1U;
        fill.block_size = 12U;
        fill.style_id = 300U + i;
        fill.flags = kTextPaintRectCaret;
        paint.fill_rects.push_back(fill);
        paint.commands.push_back({
            TextPaintCommandKind::CaretRect,
            selection_count + i,
            i % clip_count,
            0U});
    }
    return paint;
}

std::uint64_t expected_upload_fence(
    const GlyphAtlasUploadExecution& upload,
    std::uint32_t page_index,
    std::uint64_t page_generation) {
    std::uint64_t result = 0U;
    for (std::size_t i = 0; i < upload.batches.size(); ++i) {
        if (upload.batches[i].page_index == page_index &&
            upload.batches[i].page_generation == page_generation) {
            result = std::max(result, upload.receipts[i].fence_value);
        }
    }
    return result;
}

void verify_case(
    const Pipeline& pipeline,
    const GlyphAtlasSubmission& atlas,
    const GlyphAtlasUploadExecution& upload,
    std::uint32_t selection_count,
    std::uint32_t glyph_command_count,
    std::uint32_t caret_count,
    std::uint32_t clip_count,
    std::uint32_t surface_variant,
    std::uint64_t frame_id,
    bool exact_limits) {
    TextPaintCommandStream paint = make_paint_stream(
        selection_count,
        glyph_command_count,
        caret_count,
        clip_count);

    const std::uint32_t expected_commands = selection_count +
        static_cast<std::uint32_t>(atlas.draw_batches.size()) + caret_count;
    const std::uint32_t expected_fills = selection_count + caret_count;

    struct ExpectedPage final {
        std::uint32_t page_index{0};
        std::uint64_t page_generation{0};
        GlyphRasterFormat format{GlyphRasterFormat::Empty};
        std::uint32_t first_batch{0};
        std::uint32_t batch_count{0};
        std::uint64_t required_upload_fence{0};
    };
    std::vector<ExpectedPage> expected_pages;
    std::vector<std::uint32_t> expected_batch_page_indices;
    expected_batch_page_indices.reserve(atlas.draw_batches.size());
    for (std::size_t batch_index = 0;
         batch_index < atlas.draw_batches.size();
         ++batch_index) {
        const GlyphAtlasDrawBatch& batch = atlas.draw_batches[batch_index];
        const GlyphAtlasDrawInstance& first_instance =
            atlas.draw_instances[batch.first_instance];
        const GlyphRasterMode mode = pipeline.working.entries[
            first_instance.working_set_key_index].key.mode;
        const GlyphRasterFormat format = mode == GlyphRasterMode::Grayscale
            ? GlyphRasterFormat::Alpha8
            : mode == GlyphRasterMode::Lcd
                ? GlyphRasterFormat::LcdRgb8
                : GlyphRasterFormat::Bgra8;
        auto page = std::find_if(
            expected_pages.begin(),
            expected_pages.end(),
            [&batch](const ExpectedPage& candidate) {
                return candidate.page_index == batch.page_index;
            });
        if (page == expected_pages.end()) {
            ExpectedPage candidate;
            candidate.page_index = batch.page_index;
            candidate.page_generation = batch.page_generation;
            candidate.format = format;
            candidate.first_batch = static_cast<std::uint32_t>(batch_index);
            candidate.batch_count = 1U;
            candidate.required_upload_fence = expected_upload_fence(
                upload, batch.page_index, batch.page_generation);
            expected_pages.push_back(candidate);
            expected_batch_page_indices.push_back(
                static_cast<std::uint32_t>(expected_pages.size() - 1U));
        } else {
            assert(page->page_generation == batch.page_generation);
            assert(page->format == format);
            ++page->batch_count;
            expected_batch_page_indices.push_back(
                static_cast<std::uint32_t>(page - expected_pages.begin()));
        }
    }

    GpuFrameSubmissionRequest request;
    request.paint_stream = &paint;
    request.working_set = &pipeline.working;
    request.atlas_submission = &atlas;
    request.upload_execution = &upload;
    request.cache = &pipeline.cache;
    request.surface.surface_id = 77U + surface_variant;
    request.surface.generation_id = 9U + surface_variant;
    request.surface.width = 640U + surface_variant * 16U;
    request.surface.height = 480U + surface_variant * 8U;
    request.frame_id = frame_id;
    const std::uint32_t slack = exact_limits ? 0U : 7U;
    request.limits.maximum_clips = clip_count + slack;
    request.limits.maximum_commands = expected_commands + slack;
    request.limits.maximum_fill_rects =
        std::max<std::uint32_t>(1U, expected_fills + slack);
    request.limits.maximum_glyph_batches =
        static_cast<std::uint32_t>(atlas.draw_batches.size()) + slack;
    request.limits.maximum_page_references =
        static_cast<std::uint32_t>(expected_pages.size()) + slack;
    request.limits.maximum_referenced_instances =
        atlas.draw_instances.size() + slack;

    GpuFrameSubmission output;
    GpuFrameSubmissionStats stats;
    GpuFrameSubmissionError error;
    assert(prepare_gpu_frame_submission(request, &output, &stats, &error));
    assert(output.surface == request.surface);
    assert(output.frame_id == frame_id);
    assert(output.atlas_generation_id == atlas.atlas_generation_id);
    assert(output.atlas_submission_epoch == atlas.submission_epoch);
    assert(output.clips.size() == clip_count);
    assert(output.commands.size() == expected_commands);
    assert(output.fill_rects.size() == expected_fills);
    assert(output.glyph_batches.size() == atlas.draw_batches.size());
    assert(output.page_references.size() == expected_pages.size());

    for (std::uint32_t i = 0U; i < selection_count; ++i) {
        assert(output.commands[i].kind == GpuFrameCommandKind::FillRect);
        assert(output.commands[i].payload_index == i);
        assert(output.commands[i].clip_index == i % clip_count);
        assert(output.fill_rects[i] == paint.fill_rects[i]);
    }

    std::uint64_t expected_required_fence = 0U;
    for (std::size_t i = 0; i < atlas.draw_batches.size(); ++i) {
        const GlyphAtlasDrawBatch& source = atlas.draw_batches[i];
        const GpuFrameGlyphBatch& batch = output.glyph_batches[i];
        const GpuFrameCommandRecord& command =
            output.commands[selection_count + i];
        assert(command.kind == GpuFrameCommandKind::GlyphBatch);
        assert(command.payload_index == i);
        assert(command.clip_index == source.clip_index);
        assert(batch.page_generation == source.page_generation);
        assert(batch.page_index == source.page_index);
        assert(batch.first_instance == source.first_instance);
        assert(batch.instance_count == source.instance_count);
        assert(batch.style_id == source.style_id);
        assert(batch.clip_index == source.clip_index);
        assert(batch.page_reference_index == expected_batch_page_indices[i]);

        const ExpectedPage& expected_page =
            expected_pages[batch.page_reference_index];
        const GpuFramePageReference& page =
            output.page_references[batch.page_reference_index];
        assert(page.page_index == expected_page.page_index);
        assert(page.page_generation == expected_page.page_generation);
        assert(page.format == expected_page.format);
        assert(page.first_batch == expected_page.first_batch);
        assert(page.batch_count == expected_page.batch_count);
        assert(page.required_upload_fence ==
            expected_page.required_upload_fence);
        expected_required_fence = std::max(
            expected_required_fence,
            expected_page.required_upload_fence);
    }

    for (std::uint32_t i = 0U; i < caret_count; ++i) {
        const std::size_t command_index =
            selection_count + atlas.draw_batches.size() + i;
        const std::size_t fill_index = selection_count + i;
        assert(output.commands[command_index].kind ==
            GpuFrameCommandKind::FillRect);
        assert(output.commands[command_index].payload_index == fill_index);
        assert(output.commands[command_index].clip_index == i % clip_count);
        assert(output.fill_rects[fill_index] ==
            paint.fill_rects[selection_count + i]);
    }

    assert(output.required_upload_fence == expected_required_fence);
    assert(stats.input_paint_commands == paint.commands.size());
    assert(stats.output_commands == expected_commands);
    assert(stats.output_fill_rects == expected_fills);
    assert(stats.output_glyph_batches == atlas.draw_batches.size());
    assert(stats.output_page_references == expected_pages.size());
    assert(stats.selection_commands == selection_count);
    assert(stats.caret_commands == caret_count);
    assert(stats.referenced_instances == atlas.draw_instances.size());
    assert(stats.required_upload_fence == expected_required_fence);
    assert(gpu_frame_submission_is_current(
        pipeline.cache, atlas, upload, output));
}

} // namespace

int main() {
    Pipeline pipeline;
    std::uint64_t cases = 0U;
    for (std::uint32_t selections = 0U; selections < 4U; ++selections) {
        for (std::uint32_t carets = 0U; carets < 3U; ++carets) {
            for (std::uint32_t glyph_commands = 1U;
                 glyph_commands <= 4U;
                 ++glyph_commands) {
                for (std::uint32_t clips = 1U; clips <= 2U; ++clips) {
                    for (std::uint32_t surface = 0U; surface < 3U; ++surface) {
                        for (std::uint64_t frame = 1U; frame <= 8U; ++frame) {
                            for (std::uint32_t exact = 0U; exact < 2U; ++exact) {
                                verify_case(
                                    pipeline,
                                    pipeline.cold_submission,
                                    pipeline.cold_upload,
                                    selections,
                                    glyph_commands,
                                    carets,
                                    clips,
                                    surface,
                                    frame,
                                    exact != 0U);
                                ++cases;
                                verify_case(
                                    pipeline,
                                    pipeline.hot_submission,
                                    pipeline.hot_upload,
                                    selections,
                                    glyph_commands,
                                    carets,
                                    clips,
                                    surface,
                                    frame + 10'000U,
                                    exact != 0U);
                                ++cases;
                            }
                        }
                    }
                }
            }
        }
    }
    assert(cases == 9'216U);
    std::cout << "gpu-atlas-frame-submission-equivalence-tests: "
              << cases << "/" << cases << " PASS\n";
    return 0;
}
