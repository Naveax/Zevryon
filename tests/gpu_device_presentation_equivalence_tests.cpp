#include "gpu_device_presentation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
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

GlyphRasterFormat format_for(std::uint32_t index, std::uint32_t variant) {
    return static_cast<GlyphRasterFormat>((index + variant) % 3U);
}

bool run_case(
    std::uint32_t texture_count,
    std::uint32_t image_count,
    bool selection,
    bool caret,
    std::uint32_t generation_variant,
    std::uint32_t surface_variant,
    std::uint32_t payload_variant,
    std::uint32_t command_variant) {
    ReferenceGpuDeviceApi api;
    const GpuDevicePresentationConfig config{
        texture_count,
        image_count,
        image_count,
        texture_count * image_count + 1U,
        64U,
        64U,
        100U + generation_variant};
    GpuDevicePresentationBackend backend(&api, config, 8192U);

    std::vector<std::byte> payload(texture_count * 16U);
    std::vector<GlyphAtlasUploadRecord> uploads(texture_count);
    std::vector<GlyphAtlasBackendUploadBatch> batches(texture_count);
    std::vector<GlyphAtlasDrawInstance> instances(texture_count);
    std::uint64_t last_upload_fence = 0U;
    for (std::uint32_t index = 0U; index < texture_count; ++index) {
        for (std::uint32_t byte = 0U; byte < 16U; ++byte) {
            payload[index * 16U + byte] = static_cast<std::byte>(
                (index * 31U + byte * 7U + generation_variant * 11U +
                 payload_variant * 19U) & 0xffU);
        }
        const GlyphRasterFormat format = format_for(index, generation_variant);
        uploads[index] = {
            9U + generation_variant,
            20U + index + generation_variant,
            index * 16U,
            16U,
            index,
            index * 4U,
            index * 2U,
            4U,
            4U,
            4U,
            index,
            format,
            0U,
            0U};
        batches[index] = {
            9U + generation_variant,
            20U + index + generation_variant,
            index,
            index,
            1U,
            format,
            0U,
            0U};
        GlyphAtlasUploadBackendError upload_error;
        if (!backend.submit(
                batches[index],
                uploads,
                payload,
                index + 1U,
                &last_upload_fence,
                &upload_error)) {
            std::cerr << upload_error.message << '\n';
            return false;
        }
        if (last_upload_fence != index + 1U) {
            return false;
        }
        instances[index].atlas_generation_id = 9U + generation_variant;
        instances[index].page_generation = 20U + index + generation_variant;
        instances[index].page_index = index;
        instances[index].width = 4U;
        instances[index].height = 4U;
        instances[index].viewport_inline_start =
            static_cast<std::int64_t>(index * 9U + command_variant);
        instances[index].viewport_block_start =
            static_cast<std::int64_t>(index * 5U + payload_variant);
        instances[index].style_id = 50U + index + command_variant;
    }

    GpuFrameSubmission frame;
    frame.surface = {
        300U + surface_variant,
        400U + surface_variant,
        640U + surface_variant,
        480U + surface_variant,
        surface_variant == 0U
            ? GpuSurfaceFormat::Bgra8Unorm
            : GpuSurfaceFormat::Rgba8Unorm,
        1U,
        0U,
        0U};
    frame.frame_id = 1U;
    frame.atlas_generation_id = 9U + generation_variant;
    frame.required_upload_fence = last_upload_fence;
    frame.clips.push_back({0, 0, 640U, 480U});
    if (selection) {
        frame.fill_rects.push_back({0, 0, 10U, 10U, 1U, 0U, 0U, kTextPaintRectSelection});
        frame.commands.push_back({GpuFrameCommandKind::FillRect, 0U, 0U, 0U});
    }
    for (std::uint32_t index = 0U; index < texture_count; ++index) {
        frame.page_references.push_back({
            20U + index + generation_variant,
            index + 1U,
            index,
            index,
            1U,
            format_for(index, generation_variant),
            0U,
            0U});
        frame.glyph_batches.push_back({
            20U + index + generation_variant,
            index,
            index,
            1U,
            50U + index + command_variant,
            0U,
            index,
            0U});
        frame.commands.push_back({GpuFrameCommandKind::GlyphBatch, index, 0U, 0U});
    }
    if (caret) {
        const std::uint32_t fill_index = static_cast<std::uint32_t>(frame.fill_rects.size());
        frame.fill_rects.push_back({20, 0, 1U, 10U, 2U, 0U, 0U, kTextPaintRectCaret});
        frame.commands.push_back({GpuFrameCommandKind::FillRect, fill_index, 0U, 0U});
    }

    GpuFrameBackendError frame_error;
    std::uint64_t cold_fence = 0U;
    if (!backend.submit(
            frame,
            instances,
            texture_count + 1U,
            last_upload_fence,
            &cold_fence,
            &frame_error)) {
        std::cerr << frame_error.message << '\n';
        return false;
    }
    std::string retire_error;
    if (!backend.retire_completed(cold_fence, &retire_error)) {
        std::cerr << retire_error << '\n';
        return false;
    }

    frame.frame_id = 2U;
    frame.required_upload_fence = 0U;
    for (GpuFramePageReference& page : frame.page_references) {
        page.required_upload_fence = 0U;
    }
    std::uint64_t hot_fence = 0U;
    if (!backend.submit(
            frame,
            instances,
            texture_count + 2U,
            0U,
            &hot_fence,
            &frame_error)) {
        std::cerr << frame_error.message << '\n';
        return false;
    }
    const auto snapshot = backend.snapshot();
    const std::uint32_t expected_commands = texture_count +
        (selection ? 1U : 0U) + (caret ? 1U : 0U);
    GpuPresentReceipt receipt;
    return require(hot_fence == cold_fence + 1U, "hot fence follows cold fence") &&
        require(snapshot.texture_count == texture_count, "exact texture count") &&
        require(snapshot.surface_image_count == image_count, "exact image count") &&
        require(snapshot.texture_allocations == texture_count, "cold allocations only") &&
        require(snapshot.present_submissions == 2U, "cold and hot presents") &&
        require(snapshot.in_flight_frame_count == 1U, "hot frame in flight") &&
        require(snapshot.texture_pin_count == texture_count, "hot texture pins") &&
        require(backend.latest_present_receipt(&receipt), "receipt exists") &&
        require(receipt.command_count == expected_commands, "exact command count") &&
        require(receipt.signal_fence_value == hot_fence, "exact receipt fence");
}

} // namespace

int main() {
    std::uint64_t passed = 0U;
    for (std::uint32_t textures = 1U; textures <= 4U; ++textures) {
        for (std::uint32_t images = 1U; images <= 3U; ++images) {
            for (std::uint32_t flags = 0U; flags < 4U; ++flags) {
                for (std::uint32_t generation = 0U; generation < 4U; ++generation) {
                    for (std::uint32_t surface = 0U; surface < 3U; ++surface) {
                        for (std::uint32_t payload = 0U; payload < 4U; ++payload) {
                            for (std::uint32_t command = 0U; command < 4U; ++command) {
                                if (!run_case(
                                        textures,
                                        images,
                                        (flags & 1U) != 0U,
                                        (flags & 2U) != 0U,
                                        generation,
                                        surface,
                                        payload,
                                        command)) {
                                    return 1;
                                }
                                ++passed;
                            }
                        }
                    }
                }
            }
        }
    }
    constexpr std::uint64_t kExpected = 9'216U;
    if (!require(passed == kExpected, "exact oracle case count")) {
        return 1;
    }
    std::cout << "GPU device presentation equivalence: "
              << passed << "/" << kExpected << " PASS\n";
    return 0;
}
