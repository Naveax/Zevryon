#include "gpu_device_presentation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
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

struct AtlasFixture final {
    std::vector<std::byte> payload;
    std::array<GlyphAtlasUploadRecord, 3> uploads{};
    std::array<GlyphAtlasBackendUploadBatch, 3> batches{};
    std::array<GlyphAtlasDrawInstance, 3> instances{};
    GpuFrameSubmission frame;

    AtlasFixture() {
        payload.resize(48U);
        for (std::size_t index = 0U; index < payload.size(); ++index) {
            payload[index] = static_cast<std::byte>((index * 17U + 3U) & 0xffU);
        }
        constexpr std::array<GlyphRasterFormat, 3> formats{
            GlyphRasterFormat::Alpha8,
            GlyphRasterFormat::LcdRgb8,
            GlyphRasterFormat::Bgra8};
        for (std::uint32_t index = 0U; index < 3U; ++index) {
            GlyphAtlasUploadRecord upload;
            upload.atlas_generation_id = 7U;
            upload.page_generation = 11U + index;
            upload.payload_offset = index * 16U;
            upload.payload_size = 16U;
            upload.page_index = index;
            upload.atlas_x = index * 8U;
            upload.atlas_y = index * 4U;
            upload.width = 4U;
            upload.height = 4U;
            upload.row_bytes = 4U;
            upload.format = formats[index];
            uploads[index] = upload;

            GlyphAtlasBackendUploadBatch batch;
            batch.atlas_generation_id = 7U;
            batch.page_generation = 11U + index;
            batch.page_index = index;
            batch.first_upload = index;
            batch.upload_count = 1U;
            batch.format = formats[index];
            batches[index] = batch;

            GlyphAtlasDrawInstance instance;
            instance.atlas_generation_id = 7U;
            instance.page_generation = 11U + index;
            instance.page_index = index;
            instance.width = 4U;
            instance.height = 4U;
            instance.style_id = 40U + index;
            instances[index] = instance;
        }

        frame.surface = {91U, 5U, 800U, 600U, GpuSurfaceFormat::Bgra8Unorm, 1U, 0U, 0U};
        frame.frame_id = 101U;
        frame.atlas_generation_id = 7U;
        frame.atlas_submission_epoch = 1U;
        frame.required_upload_fence = 3U;
        frame.clips.push_back({0, 0, 800U, 600U});

        TextPaintFillRect selection;
        selection.inline_size = 20U;
        selection.block_size = 16U;
        selection.style_id = 1U;
        selection.flags = kTextPaintRectSelection;
        frame.fill_rects.push_back(selection);

        TextPaintFillRect caret;
        caret.viewport_inline_start = 30;
        caret.inline_size = 1U;
        caret.block_size = 16U;
        caret.style_id = 2U;
        caret.flags = kTextPaintRectCaret;
        frame.fill_rects.push_back(caret);

        frame.commands.push_back({GpuFrameCommandKind::FillRect, 0U, 0U, 0U});
        for (std::uint32_t index = 0U; index < 3U; ++index) {
            GpuFramePageReference page;
            page.page_generation = 11U + index;
            page.required_upload_fence = 1U + index;
            page.page_index = index;
            page.first_batch = index;
            page.batch_count = 1U;
            page.format = formats[index];
            frame.page_references.push_back(page);

            GpuFrameGlyphBatch batch;
            batch.page_generation = 11U + index;
            batch.page_index = index;
            batch.first_instance = index;
            batch.instance_count = 1U;
            batch.style_id = 40U + index;
            batch.clip_index = 0U;
            batch.page_reference_index = index;
            frame.glyph_batches.push_back(batch);
            frame.commands.push_back({GpuFrameCommandKind::GlyphBatch, index, 0U, 0U});
        }
        frame.commands.push_back({GpuFrameCommandKind::FillRect, 1U, 0U, 0U});
    }
};

GpuDevicePresentationConfig config(
    std::uint32_t textures = 3U,
    std::uint32_t images = 2U,
    std::uint32_t frames = 2U,
    std::uint32_t pins = 8U) {
    return {textures, images, frames, pins, 64U, 64U, 77U};
}

bool upload_cold(
    GpuDevicePresentationBackend* backend,
    const AtlasFixture& fixture,
    std::array<std::uint64_t, 3>* fences) {
    for (std::uint32_t index = 0U; index < 3U; ++index) {
        GlyphAtlasUploadBackendError error;
        if (!require(
                backend->submit(
                    fixture.batches[index],
                    fixture.uploads,
                    fixture.payload,
                    index + 1U,
                    &(*fences)[index],
                    &error),
                error.message.c_str()) ||
            !require((*fences)[index] == index + 1U, "monotone upload fence")) {
            return false;
        }
    }
    return true;
}

bool test_cold_hot_present_and_retire() {
    ReferenceGpuDeviceApi api;
    GpuDevicePresentationBackend backend(&api, config(), 4096U);
    AtlasFixture fixture;
    std::array<std::uint64_t, 3> upload_fences{};
    if (!upload_cold(&backend, fixture, &upload_fences)) {
        return false;
    }

    GpuFrameBackendError error;
    std::uint64_t cold_fence = 0U;
    if (!require(
            backend.submit(
                fixture.frame,
                fixture.instances,
                4U,
                3U,
                &cold_fence,
                &error),
            error.message.c_str()) ||
        !require(cold_fence == 4U, "cold present fence") ||
        !require(backend.snapshot().texture_pin_count == 3U, "cold frame pins textures")) {
        return false;
    }

    std::string retire_error;
    if (!require(backend.retire_completed(cold_fence, &retire_error), retire_error.c_str())) {
        return false;
    }
    auto snapshot = backend.snapshot();
    if (!require(snapshot.texture_count == 3U, "three textures retained") ||
        !require(snapshot.in_flight_frame_count == 0U, "cold frame retired") ||
        !require(snapshot.texture_pin_count == 0U, "cold pins released")) {
        return false;
    }

    fixture.frame.frame_id = 102U;
    fixture.frame.required_upload_fence = 0U;
    for (auto& page : fixture.frame.page_references) {
        page.required_upload_fence = 0U;
    }
    std::uint64_t hot_fence = 0U;
    if (!require(
            backend.submit(
                fixture.frame,
                fixture.instances,
                5U,
                0U,
                &hot_fence,
                &error),
            error.message.c_str()) ||
        !require(hot_fence == 5U, "hot resident present fence") ||
        !require(backend.snapshot().texture_allocations == 3U, "hot frame allocates no textures")) {
        return false;
    }

    GpuPresentReceipt receipt;
    if (!require(backend.latest_present_receipt(&receipt), "latest receipt available") ||
        !require(receipt.signal_fence_value == hot_fence, "latest receipt fence") ||
        !require(receipt.status == GpuPresentReceiptStatus::Submitted, "receipt submitted")) {
        return false;
    }
    if (!require(!backend.retire_completed(hot_fence + 1U, &retire_error),
                 "completion beyond submitted timeline rejected") ||
        !require(backend.retire_completed(hot_fence, &retire_error), retire_error.c_str()) ||
        !require(backend.latest_present_receipt(&receipt), "retired receipt available") ||
        !require(receipt.status == GpuPresentReceiptStatus::Retired, "receipt retired") ||
        !require(backend.snapshot().last_submitted_fence_value == hot_fence,
                 "global submitted fence retained")) {
        return false;
    }
    return true;
}

bool test_surface_and_ring_fail_closed() {
    ReferenceGpuDeviceApi api;
    GpuDevicePresentationBackend backend(&api, config(1U, 1U, 1U, 2U), 2048U);
    AtlasFixture fixture;
    std::uint64_t upload_fence = 0U;
    GlyphAtlasUploadBackendError upload_error;
    if (!require(
            backend.submit(
                fixture.batches[0], fixture.uploads, fixture.payload,
                1U, &upload_fence, &upload_error),
            upload_error.message.c_str())) {
        return false;
    }
    GpuFrameSubmission frame;
    frame.surface = fixture.frame.surface;
    frame.frame_id = 1U;
    frame.atlas_generation_id = 7U;
    frame.clips.push_back({0, 0, 100U, 100U});
    frame.fill_rects.push_back(fixture.frame.fill_rects[0]);
    frame.commands.push_back({GpuFrameCommandKind::FillRect, 0U, 0U, 0U});
    frame.page_references.push_back(fixture.frame.page_references[0]);
    frame.glyph_batches.push_back(fixture.frame.glyph_batches[0]);
    frame.commands.push_back({GpuFrameCommandKind::GlyphBatch, 0U, 0U, 0U});
    std::array<GlyphAtlasDrawInstance, 1> instances{fixture.instances[0]};

    GpuFrameBackendError frame_error;
    std::uint64_t fence = 0U;
    if (!require(
            backend.submit(frame, instances, 2U, upload_fence, &fence, &frame_error),
            frame_error.message.c_str())) {
        return false;
    }
    frame.frame_id = 2U;
    std::uint64_t ignored = 0U;
    if (!require(
            !backend.submit(frame, instances, 3U, upload_fence, &ignored, &frame_error),
            "ring exhaustion rejected")) {
        return false;
    }
    frame.surface.generation_id = 6U;
    if (!require(
            !backend.submit(frame, instances, 4U, upload_fence, &ignored, &frame_error),
            "surface reconfigure while in flight rejected")) {
        return false;
    }
    std::string clear_error;
    if (!require(!backend.clear(&clear_error), "clear while in flight rejected")) {
        return false;
    }
    if (!require(backend.retire_completed(fence, &clear_error), clear_error.c_str()) ||
        !require(backend.clear(&clear_error), clear_error.c_str()) ||
        !require(backend.snapshot().config.device_generation == 78U, "clear increments device generation")) {
        return false;
    }
    return true;
}

bool test_pinned_eviction_and_payload_validation() {
    ReferenceGpuDeviceApi api;
    GpuDevicePresentationBackend backend(&api, config(1U, 1U, 1U, 2U), 2048U);
    AtlasFixture fixture;
    std::uint64_t first_upload = 0U;
    GlyphAtlasUploadBackendError upload_error;
    if (!require(
            backend.submit(
                fixture.batches[0], fixture.uploads, fixture.payload,
                1U, &first_upload, &upload_error),
            upload_error.message.c_str())) {
        return false;
    }

    GpuFrameSubmission frame;
    frame.surface = fixture.frame.surface;
    frame.frame_id = 1U;
    frame.atlas_generation_id = 7U;
    frame.clips.push_back({0, 0, 100U, 100U});
    frame.fill_rects.push_back(fixture.frame.fill_rects[0]);
    frame.commands.push_back({GpuFrameCommandKind::FillRect, 0U, 0U, 0U});
    frame.page_references.push_back(fixture.frame.page_references[0]);
    frame.glyph_batches.push_back(fixture.frame.glyph_batches[0]);
    frame.commands.push_back({GpuFrameCommandKind::GlyphBatch, 0U, 0U, 0U});
    std::array<GlyphAtlasDrawInstance, 1> instances{fixture.instances[0]};
    GpuFrameBackendError frame_error;
    std::uint64_t present_fence = 0U;
    if (!require(
            backend.submit(frame, instances, 2U, first_upload, &present_fence, &frame_error),
            frame_error.message.c_str())) {
        return false;
    }

    std::uint64_t second_upload = 0U;
    if (!require(
            !backend.submit(
                fixture.batches[0], fixture.uploads, fixture.payload,
                3U, &second_upload, &upload_error),
            "pinned texture cannot be uploaded in place") ||
        !require(
            !backend.submit(
                fixture.batches[1], fixture.uploads, fixture.payload,
                4U, &second_upload, &upload_error),
            "pinned texture cannot be evicted")) {
        return false;
    }
    std::string retire_error;
    if (!require(backend.retire_completed(present_fence, &retire_error), retire_error.c_str()) ||
        !require(
            backend.submit(
                fixture.batches[1], fixture.uploads, fixture.payload,
                5U, &second_upload, &upload_error),
            upload_error.message.c_str()) ||
        !require(backend.snapshot().texture_evictions == 1U, "unpinned LRU texture evicted")) {
        return false;
    }

    auto malformed = fixture.uploads;
    malformed[2].payload_offset = 1'000U;
    std::uint64_t bad_fence = 0U;
    if (!require(
            !backend.submit(
                fixture.batches[2], malformed, fixture.payload,
                6U, &bad_fence, &upload_error),
            "invalid payload is rejected")) {
        return false;
    }
    return true;
}

bool test_metadata_budget_failure() {
    ReferenceGpuDeviceApi api;
    GpuDevicePresentationBackend backend(&api, config(), 1U);
    AtlasFixture fixture;
    std::uint64_t fence = 0U;
    GlyphAtlasUploadBackendError error;
    return require(
        !backend.submit(
            fixture.batches[0], fixture.uploads, fixture.payload,
            1U, &fence, &error),
        "metadata constructor failure leaves backend invalid");
}

} // namespace

int main() {
    if (!test_cold_hot_present_and_retire() ||
        !test_surface_and_ring_fail_closed() ||
        !test_pinned_eviction_and_payload_validation() ||
        !test_metadata_budget_failure()) {
        return 1;
    }
    std::cout << "GPU device presentation tests passed\n";
    return 0;
}
