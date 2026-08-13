#include "native_platform_adapters.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

using namespace zevryon::text;

namespace {

struct Fixture final {
    std::array<std::byte, 64U * 1024U> storage{};
    std::pmr::monotonic_buffer_resource arena{
        storage.data(), storage.size(), std::pmr::null_memory_resource()};
    GpuFrameSubmission frame{&arena};
    NativeCommandBuffer commands{&arena};
    std::pmr::vector<GlyphAtlasDrawInstance> draws{&arena};

    Fixture() {
        frame.surface.surface_id = 41U;
        frame.surface.generation_id = 7U;
        frame.surface.width = 1280U;
        frame.surface.height = 720U;
        frame.frame_id = 9001U;
        frame.atlas_generation_id = 11U;
        frame.required_upload_fence = 12U;

        TextPaintFillRect fill;
        fill.viewport_inline_start = 10;
        fill.viewport_block_start = 20;
        fill.inline_size = 100U;
        fill.block_size = 30U;
        fill.style_id = 3U;
        frame.fill_rects.push_back(fill);

        GpuFramePageReference page;
        page.page_generation = 19U;
        page.required_upload_fence = 12U;
        page.page_index = 2U;
        page.first_batch = 0U;
        page.batch_count = 1U;
        page.format = GlyphRasterFormat::Alpha8;
        frame.page_references.push_back(page);

        GpuFrameGlyphBatch batch;
        batch.page_generation = 19U;
        batch.page_index = 2U;
        batch.first_instance = 0U;
        batch.instance_count = 1U;
        batch.style_id = 4U;
        batch.page_reference_index = 0U;
        frame.glyph_batches.push_back(batch);

        GlyphAtlasDrawInstance draw;
        draw.page_generation = 19U;
        draw.page_index = 2U;
        draw.width = 8U;
        draw.height = 12U;
        draws.push_back(draw);

        commands.surface = frame.surface;
        commands.frame_id = frame.frame_id;
        commands.command_generation = 5U;
        commands.command_checksum = 0x123456789ABCDEF0ULL;
        commands.damage_rects.push_back({0, 0, 1280U, 720U});
        commands.commands.push_back({NativeCommandKind::BeginRenderPass, 0U, 0U, 0U});
        commands.commands.push_back({NativeCommandKind::SetScissor, 0U, 0U, 0U});
        commands.commands.push_back({NativeCommandKind::FillRect, 0U, 0U, 0U});
        commands.commands.push_back({NativeCommandKind::GlyphBatch, 0U, 0U, 0U});
        commands.commands.push_back({NativeCommandKind::EndRenderPass, 0U, 0U, 0U});
    }
};

NativePlatformAdapterConfig config_for(NativeGpuApiKind kind) {
    NativePlatformAdapterConfig config;
    config.api_kind = kind;
    config.maximum_commands = 64U;
    config.maximum_barriers = 8U;
    config.maximum_descriptors = 8U;
    config.maximum_swapchain_images = 3U;
    config.maximum_frames_in_flight = 2U;
    config.flags = kNativePlatformRequireTimelineFence |
        kNativePlatformAllowMailbox |
        kNativePlatformAllowImmediate |
        kNativePlatformAllowTearing;
    config.device_generation = 17U;
    config.driver_generation = 29U;
    return config;
}

NativePlatformSwapchainImage image_for(const Fixture& fixture) {
    NativePlatformSwapchainImage image;
    image.image.device_generation = 17U;
    image.image.surface_id = fixture.frame.surface.surface_id;
    image.image.surface_generation = fixture.frame.surface.generation_id;
    image.image.image_generation = 31U;
    image.image.image_index = 0U;
    image.image.flags = 1U;
    image.driver_generation = 29U;
    image.native_resource_id = 43U;
    image.state = NativePlatformResourceState::Present;
    return image;
}

void test_compile_supported_backends() {
    for (NativeGpuApiKind kind : {
             NativeGpuApiKind::Vulkan,
             NativeGpuApiKind::Direct3D12}) {
        Fixture fixture;
        std::array<std::byte, 32U * 1024U> output_storage{};
        std::pmr::monotonic_buffer_resource output_arena{
            output_storage.data(), output_storage.size(), std::pmr::null_memory_resource()};
        NativePlatformSubmission output(&output_arena);
        NativePlatformCompileRequest request;
        request.commands = &fixture.commands;
        request.frame = &fixture.frame;
        request.draw_instances = fixture.draws;
        request.image = image_for(fixture);
        request.ticket_id = 1U;
        request.wait_fence_value = 12U;
        request.config = config_for(kind);
        NativePlatformCompileStats stats;
        NativePlatformCompileError error;
        assert(compile_native_platform_submission(request, &output, &stats, &error));
        assert(error.kind == NativePlatformCompileErrorKind::None);
        assert(output.api_kind == kind);
        assert(output.commands.size() == 12U);
        assert(output.barriers.size() == 2U);
        assert(output.descriptors.size() == 1U);
        assert(stats.fill_commands == 1U);
        assert(stats.glyph_draw_commands == 1U);
        assert(stats.scissor_commands == 1U);
        assert(stats.waited_pages == 1U);
        assert(output.encoded_checksum != 0U);
        assert(output.commands.front().kind == NativePlatformCommandKind::BeginCommandBuffer);
        assert(output.commands.back().kind == NativePlatformCommandKind::Present);
    }
}

void test_metal_unsupported() {
    Fixture fixture;
    std::array<std::byte, 32U * 1024U> output_storage{};
    std::pmr::monotonic_buffer_resource output_arena{
        output_storage.data(), output_storage.size(), std::pmr::null_memory_resource()};
    NativePlatformSubmission output(&output_arena);
    NativePlatformCompileRequest request;
    request.commands = &fixture.commands;
    request.frame = &fixture.frame;
    request.draw_instances = fixture.draws;
    request.image = image_for(fixture);
    request.ticket_id = 1U;
    request.wait_fence_value = 12U;
    request.config = config_for(NativeGpuApiKind::Metal);
    NativePlatformCompileError compile_error;
    assert(!compile_native_platform_submission(
        request, &output, nullptr, &compile_error));
    assert(compile_error.kind == NativePlatformCompileErrorKind::UnsupportedCapability);
    assert(output.commands.empty());
    assert(output.barriers.empty());
    assert(output.descriptors.empty());

    const NativePlatformCapabilities capabilities =
        default_native_platform_capabilities(NativeGpuApiKind::Metal);
    assert(capabilities.flags == 0U);
    assert(capabilities.maximum_commands == 0U);
    assert(capabilities.maximum_barriers == 0U);
    assert(capabilities.maximum_descriptors == 0U);
    assert(capabilities.maximum_swapchain_images == 0U);
    assert(capabilities.maximum_frames_in_flight == 0U);
    assert(capabilities.maximum_staging_bytes == 0U);

    ReferenceNativePlatformDriver driver(NativeGpuApiKind::Metal);
    assert(driver.capabilities() == capabilities);
    NativeGpuApiError api_error;
    assert(!driver.configure_swapchain(
        fixture.frame.surface,
        3U,
        config_for(NativeGpuApiKind::Metal),
        &api_error));
    assert(api_error.kind == NativeGpuApiErrorKind::InvalidInput);

    MetalNativeGpuCommandApi legacy_api(
        &driver, config_for(NativeGpuApiKind::Metal));
    assert(legacy_api.kind() == NativeGpuApiKind::Metal);
    assert(legacy_api.capabilities() == capabilities);
    assert(!legacy_api.configure_surface(
        fixture.frame.surface, 3U, 17U, &api_error));
}

void test_fail_closed_inputs() {
    Fixture fixture;
    std::array<std::byte, 32U * 1024U> output_storage{};
    std::pmr::monotonic_buffer_resource output_arena{
        output_storage.data(), output_storage.size(), std::pmr::null_memory_resource()};
    NativePlatformSubmission output(&output_arena);
    NativePlatformCompileRequest request;
    request.commands = &fixture.commands;
    request.frame = &fixture.frame;
    request.draw_instances = fixture.draws;
    request.image = image_for(fixture);
    request.ticket_id = 1U;
    request.wait_fence_value = 11U;
    request.config = config_for(NativeGpuApiKind::Vulkan);
    NativePlatformCompileError error;
    assert(!compile_native_platform_submission(request, &output, nullptr, &error));
    assert(error.kind == NativePlatformCompileErrorKind::UploadFenceNotReady);
    assert(output.commands.empty());

    request.wait_fence_value = 12U;
    request.image.image.surface_generation += 1U;
    assert(!compile_native_platform_submission(request, &output, nullptr, &error));
    assert(error.kind == NativePlatformCompileErrorKind::StaleSwapchainImage);
    assert(output.commands.empty());

    request.image = image_for(fixture);
    request.config.maximum_commands = 4U;
    assert(!compile_native_platform_submission(request, &output, nullptr, &error));
    assert(error.kind == NativePlatformCompileErrorKind::CommandCapacityExceeded);
    assert(output.commands.empty());
}

void test_adapter_lifecycle() {
    for (NativeGpuApiKind kind : {
             NativeGpuApiKind::Vulkan,
             NativeGpuApiKind::Direct3D12}) {
        Fixture fixture;
        ReferenceNativePlatformDriver driver(kind);
        NativePlatformAdapterConfig config = config_for(kind);
        NativePlatformGpuCommandApi api(&driver, config);
        NativeGpuApiError error;
        assert(api.kind() == kind);
        assert(api.configure_surface(fixture.frame.surface, 3U, 17U, &error));

        NativeSwapchainImageHandle image;
        NativeAcquireStatus acquire_status = NativeAcquireStatus::NotReady;
        assert(api.acquire_next_image(
            fixture.frame.surface,
            NativePresentMode::Fifo,
            1U,
            &image,
            &acquire_status,
            &error));
        assert(acquire_status == NativeAcquireStatus::Acquired);
        assert(image.device_generation == 17U);

        std::uint64_t fence = 0U;
        std::uint64_t checksum = 0U;
        NativePresentStatus present_status = NativePresentStatus::SkippedNoDamage;
        assert(api.encode_submit_present(
            image,
            fixture.commands,
            fixture.frame,
            fixture.draws,
            1U,
            12U,
            &fence,
            &checksum,
            &present_status,
            &error));
        assert(present_status == NativePresentStatus::Presented);
        assert(fence == 1U);
        assert(checksum != 0U);

        driver.set_next_acquire_status(NativeAcquireStatus::OutOfDate);
        assert(api.acquire_next_image(
            fixture.frame.surface,
            NativePresentMode::Fifo,
            2U,
            &image,
            &acquire_status,
            &error));
        assert(acquire_status == NativeAcquireStatus::OutOfDate);
    }
}

void test_backend_capabilities() {
    const NativePlatformCapabilities vk =
        default_native_platform_capabilities(NativeGpuApiKind::Vulkan);
    const NativePlatformCapabilities metal =
        default_native_platform_capabilities(NativeGpuApiKind::Metal);
    const NativePlatformCapabilities d3d =
        default_native_platform_capabilities(NativeGpuApiKind::Direct3D12);
    assert((vk.flags & kNativePlatformExplicitBarriers) != 0U);
    assert(metal.flags == 0U);
    assert(metal.maximum_commands == 0U);
    assert(metal.maximum_swapchain_images == 0U);
    assert((d3d.flags & kNativePlatformTearing) != 0U);
}

} // namespace

int main() {
    test_compile_supported_backends();
    test_metal_unsupported();
    test_fail_closed_inputs();
    test_adapter_lifecycle();
    test_backend_capabilities();
    return 0;
}
