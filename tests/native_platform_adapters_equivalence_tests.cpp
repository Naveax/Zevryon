#include "native_platform_adapters.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <vector>

using namespace zevryon::text;

namespace {

NativePlatformAdapterConfig config_for(NativeGpuApiKind kind) {
    NativePlatformAdapterConfig config;
    config.api_kind = kind;
    config.maximum_commands = 256U;
    config.maximum_barriers = 8U;
    config.maximum_descriptors = 16U;
    config.maximum_swapchain_images = 3U;
    config.maximum_frames_in_flight = 2U;
    config.flags = kNativePlatformRequireTimelineFence |
        kNativePlatformAllowMailbox |
        kNativePlatformAllowImmediate |
        kNativePlatformAllowTearing;
    config.device_generation = 7U;
    config.driver_generation = 9U;
    return config;
}

NativePlatformSwapchainImage image_for(const GpuSurfaceDescriptor& surface) {
    NativePlatformSwapchainImage image;
    image.image.device_generation = 7U;
    image.image.surface_id = surface.surface_id;
    image.image.surface_generation = surface.generation_id;
    image.image.image_generation = 17U;
    image.image.image_index = 1U;
    image.image.flags = 1U;
    image.driver_generation = 9U;
    image.native_resource_id = 23U;
    image.state = NativePlatformResourceState::Present;
    return image;
}

std::vector<NativePlatformCommandKind> command_kinds(
    const NativePlatformSubmission& submission) {
    std::vector<NativePlatformCommandKind> kinds;
    kinds.reserve(submission.commands.size());
    for (const NativePlatformCommandRecord& command : submission.commands) {
        kinds.push_back(command.kind);
    }
    return kinds;
}

} // namespace

int main() {
    std::uint64_t cases = 0U;
    for (std::uint32_t mask = 0U; mask < 1024U; ++mask) {
        for (std::uint32_t damage_count = 1U; damage_count <= 3U; ++damage_count) {
            std::array<std::byte, 128U * 1024U> storage{};
            std::pmr::monotonic_buffer_resource arena{
                storage.data(), storage.size(), std::pmr::null_memory_resource()};
            GpuFrameSubmission frame(&arena);
            frame.surface.surface_id = 101U;
            frame.surface.generation_id = 5U;
            frame.surface.width = 1920U;
            frame.surface.height = 1080U;
            frame.frame_id = 77U;
            frame.atlas_generation_id = 13U;
            frame.required_upload_fence = 31U;

            for (std::uint32_t index = 0U; index < 4U; ++index) {
                TextPaintFillRect fill;
                fill.viewport_inline_start = static_cast<std::int64_t>(index * 20U);
                fill.viewport_block_start = static_cast<std::int64_t>(index * 10U);
                fill.inline_size = 18U;
                fill.block_size = 8U;
                fill.style_id = index + 1U;
                frame.fill_rects.push_back(fill);
            }
            for (std::uint32_t index = 0U; index < 2U; ++index) {
                GpuFramePageReference page;
                page.page_generation = 40U + index;
                page.required_upload_fence = 30U + index;
                page.page_index = index;
                page.first_batch = index;
                page.batch_count = 1U;
                page.format = index == 0U ? GlyphRasterFormat::Alpha8 : GlyphRasterFormat::Bgra8;
                frame.page_references.push_back(page);

                GpuFrameGlyphBatch batch;
                batch.page_generation = page.page_generation;
                batch.page_index = page.page_index;
                batch.first_instance = index;
                batch.instance_count = 1U;
                batch.style_id = 10U + index;
                batch.page_reference_index = index;
                frame.glyph_batches.push_back(batch);
            }

            std::pmr::vector<GlyphAtlasDrawInstance> draws(&arena);
            for (std::uint32_t index = 0U; index < 2U; ++index) {
                GlyphAtlasDrawInstance draw;
                draw.page_generation = 40U + index;
                draw.page_index = index;
                draw.width = 12U;
                draw.height = 16U;
                draws.push_back(draw);
            }

            NativeCommandBuffer commands(&arena);
            commands.surface = frame.surface;
            commands.frame_id = frame.frame_id;
            commands.command_generation = 3U;
            commands.command_checksum = 0xA5A50000ULL + mask;
            commands.commands.push_back({NativeCommandKind::BeginRenderPass, 0U, 0U, 0U});
            std::size_t expected_translated = 0U;
            std::array<bool, 2U> glyph_page_used{false, false};
            for (std::uint32_t damage = 0U; damage < damage_count; ++damage) {
                commands.damage_rects.push_back({
                    static_cast<std::int64_t>(damage * 100U),
                    static_cast<std::int64_t>(damage * 50U),
                    80U,
                    40U});
                commands.commands.push_back({NativeCommandKind::SetScissor, damage, damage, kNativeCommandPartialDamage});
                ++expected_translated;
                bool emitted = false;
                for (std::uint32_t fill = 0U; fill < 4U; ++fill) {
                    if ((mask & (1U << ((fill + damage) % 10U))) != 0U) {
                        commands.commands.push_back({NativeCommandKind::FillRect, fill, damage, kNativeCommandPartialDamage});
                        ++expected_translated;
                        emitted = true;
                    }
                }
                for (std::uint32_t glyph = 0U; glyph < 2U; ++glyph) {
                    if ((mask & (1U << ((glyph + damage + 4U) % 10U))) != 0U) {
                        const std::uint32_t flags = kNativeCommandPartialDamage |
                            (damage != 0U ? kNativeCommandDuplicatedAcrossDamage : 0U);
                        commands.commands.push_back({NativeCommandKind::GlyphBatch, glyph, damage, flags});
                        expected_translated += 2U;
                        glyph_page_used[glyph] = true;
                        emitted = true;
                    }
                }
                if (!emitted) {
                    commands.commands.push_back({NativeCommandKind::FillRect, damage % 4U, damage, kNativeCommandPartialDamage});
                    ++expected_translated;
                }
            }
            commands.commands.push_back({NativeCommandKind::EndRenderPass, 0U, 0U, 0U});

            std::vector<NativePlatformCommandKind> canonical;
            std::size_t canonical_descriptors = 0U;
            for (NativeGpuApiKind kind : {
                     NativeGpuApiKind::Vulkan,
                     NativeGpuApiKind::Direct3D12}) {
                std::array<std::byte, 128U * 1024U> output_storage{};
                std::pmr::monotonic_buffer_resource output_arena{
                    output_storage.data(), output_storage.size(), std::pmr::null_memory_resource()};
                NativePlatformSubmission output(&output_arena);
                NativePlatformCompileRequest request;
                request.commands = &commands;
                request.frame = &frame;
                request.draw_instances = draws;
                request.image = image_for(frame.surface);
                request.ticket_id = 19U;
                request.wait_fence_value = 31U;
                request.config = config_for(kind);
                NativePlatformCompileStats stats;
                NativePlatformCompileError error;
                assert(compile_native_platform_submission(request, &output, &stats, &error));
                assert(output.commands.size() == expected_translated + 8U);
                assert(output.barriers.size() == 2U);
                const std::size_t expected_descriptors =
                    static_cast<std::size_t>(glyph_page_used[0]) +
                    static_cast<std::size_t>(glyph_page_used[1]);
                assert(output.descriptors.size() == expected_descriptors);
                assert(output.encoded_checksum != 0U);
                if (canonical.empty()) {
                    canonical = command_kinds(output);
                    canonical_descriptors = output.descriptors.size();
                } else {
                    assert(command_kinds(output) == canonical);
                    assert(output.descriptors.size() == canonical_descriptors);
                }
                ++cases;
            }
        }
    }
    assert(cases == 6144U);
    std::cout << "native platform adapter oracle: " << cases << "/6144 PASS\n";
    return 0;
}
