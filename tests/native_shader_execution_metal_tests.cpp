#include "native_metal_window.hpp"
#include "native_metal_window_test_window.hpp"
#include "native_shader_execution.hpp"
#include "shader_draw_packet_fixture.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

namespace {
using namespace zevryon::text;
using namespace zevryon::text::test;

struct PacketSet final {
    std::vector<std::byte> cold_storage;
    std::vector<std::byte> hot_storage;
    std::pmr::monotonic_buffer_resource cold_resource;
    std::pmr::monotonic_buffer_resource hot_resource;
    GpuShaderPacket cold;
    GpuShaderPacket hot;
    ShaderAtlasResidency atlas;
    ShaderReadback reference;

    PacketSet()
        : cold_storage(2U * 1024U * 1024U),
          hot_storage(2U * 1024U * 1024U),
          cold_resource(cold_storage.data(), cold_storage.size()),
          hot_resource(hot_storage.data(), hot_storage.size()),
          cold(&cold_resource),
          hot(&hot_resource),
          atlas(8U, 1U << 20U) {}
};

bool build_packets(PacketSet* output) {
    if (output == nullptr) {
        return false;
    }
    ShaderPacketFixture fixture = make_shader_packet_fixture();
    ShaderPacketError error;
    if (!compile_gpu_shader_packet(fixture.input(), &output->cold, &error) ||
        !output->atlas.apply_packet_uploads(output->cold, &error) ||
        !execute_shader_packet_reference(
            output->cold, output->atlas, &output->reference, &error)) {
        std::cerr << "packet preparation failed: " << error.message << '\n';
        return false;
    }
    fixture.uploads.clear();
    fixture.payload.clear();
    if (!compile_gpu_shader_packet(
            fixture.input(2U, 7U), &output->hot, &error)) {
        std::cerr << "hot packet preparation failed: " << error.message << '\n';
        return false;
    }
    return true;
}

NativeGpuSdkConfig owner_config(const NativeWindowSurfaceHandle& window) {
    NativeGpuSdkConfig config{};
    config.api_kind = NativeGpuApiKind::Metal;
    config.allow_software_device = 0U;
    config.require_real_device = 1U;
    config.device_generation = 311U;
    config.runtime_generation = 313U;
    config.window = window;
    config.limits.maximum_swapchain_images = 4U;
    config.limits.maximum_frames_in_flight = 2U;
    config.limits.maximum_device_local_bytes = 128U * 1024U * 1024U;
    return config;
}

NativeWindowSwapchainConfig swapchain_config(
    const NativeGpuSdkContextHandle& context,
    const NativeWindowSurfaceHandle& window) {
    NativeWindowSwapchainConfig config{};
    config.context = context;
    config.window = window;
    config.surface.surface_id = 0x4D4554414CULL;
    config.surface.generation_id = 1U;
    config.surface.width = 640U;
    config.surface.height = 360U;
    config.surface.format = GpuSurfaceFormat::Bgra8Unorm;
    config.surface.premultiplied_alpha = 1U;
    config.swapchain_generation = 1U;
    config.present_mode = NativePresentMode::Fifo;
    config.image_count = 3U;
    config.flags = kNativeWindowSwapchainRequireNativeContext;
    config.limits = default_native_window_swapchain_limits(
        NativeGpuApiKind::Metal,
        NativeWindowSystem::CocoaLayer);
    config.limits.maximum_image_count = 3U;
    config.limits.maximum_frames_in_flight = 2U;
    config.limits.maximum_damage_rects = 64U;
    return config;
}

NativeWindowPixelBufferView reference_pixels(const ShaderReadback& reference) {
    NativeWindowPixelBufferView view{};
    view.bytes = std::span<const std::byte>(reference.bgra);
    view.width = reference.width;
    view.height = reference.height;
    view.row_bytes = reference.row_bytes;
    view.format = GpuSurfaceFormat::Bgra8Unorm;
    view.premultiplied_alpha = 1U;
    view.checksum = reference.checksum;
    return view;
}

bool readbacks_equal(const ShaderReadback& left, const ShaderReadback& right) {
    return left.width == right.width && left.height == right.height &&
        left.row_bytes == right.row_bytes && left.checksum == right.checksum &&
        left.bgra.size() == right.bgra.size() &&
        std::equal(left.bgra.begin(), left.bgra.end(), right.bgra.begin());
}

} // namespace

int main() {
    assert(native_metal_window_build_has_backend(
        NativeWindowSystem::CocoaLayer));
    assert(native_shader_execution_build_has_backend(NativeGpuApiKind::Metal));

    PacketSet packets;
    assert(build_packets(&packets));
    assert(packets.cold.header.command_count == 68U);
    assert(packets.cold.header.fill_instance_count == 65U);
    assert(packets.cold.header.glyph_instance_count == 240U);
    assert(packets.atlas.resident_pages() == 3U);
    assert(packets.atlas.resident_bytes() == 49'152U);

    std::unique_ptr<MetalWindowTestHost> window =
        make_metal_window_test_host(640U, 360U);
    assert(window != nullptr);
    window->set_visible(true);
    window->pump_events();

    std::unique_ptr<NativeGpuSdkApi> owner =
        make_metal_window_native_gpu_sdk_api();
    assert(owner != nullptr);
    NativeGpuSdkError sdk_error;
    assert(owner->initialize(owner_config(window->handle()), &sdk_error));
    NativeGpuSdkContextHandle context;
    assert(owner->export_context(&context, &sdk_error));

    std::unique_ptr<NativeShaderExecutor> executor =
        make_metal_native_shader_executor();
    assert(executor != nullptr);
    NativeShaderExecutionConfig config{};
    config.context = context;
    config.limits = default_native_shader_execution_limits(
        NativeGpuApiKind::Metal);
    config.executor_generation = 317U;
    NativeShaderExecutionError error;
    assert(executor->configure(config, &error));

    std::unique_ptr<NativeWindowSwapchainApi> presenter =
        make_metal_native_window_swapchain_api();
    assert(presenter != nullptr);
    NativeWindowSwapchainError present_error;
    assert(presenter->configure(
        swapchain_config(context, window->handle()), &present_error));

    NativeShaderExecutionSnapshot snapshot = executor->snapshot();
    assert((snapshot.capability_flags &
        kNativeShaderExecutionDirectSurfaceExport) != 0U);

    assert(executor->execute(
        packets.cold, packets.atlas, nullptr, &error));
    NativeShaderSurfaceView direct_surface;
    assert(executor->export_surface(&direct_surface, &error));
    assert(native_shader_surface_view_valid(direct_surface));
    assert(direct_surface.api_kind == NativeGpuApiKind::Metal);

    snapshot = executor->snapshot();
    assert(snapshot.executions == 1U);
    assert(snapshot.readbacks == 0U);
    assert(snapshot.peak_transient_bytes == 0U);

    const std::array<NativeDamageRect, 1U> full{{{0, 0, 640U, 360U}}};
    NativeWindowSwapchainImage image{};
    NativeWindowAcquireStatus acquire_status{};
    assert(presenter->acquire(401U, &image, &acquire_status, &present_error));
    assert(acquire_status == NativeWindowAcquireStatus::Acquired);

    NativeWindowPresentRequest mixed{};
    mixed.image = image;
    mixed.damage_rects = full;
    mixed.frame_id = direct_surface.frame_id;
    mixed.ticket_id = 401U;
    mixed.command_checksum = direct_surface.content_checksum;
    mixed.command_count = packets.cold.header.command_count;
    mixed.flags = kNativeWindowPresentFullRedraw;
    mixed.shader_surface = direct_surface;
    mixed.pixel_buffer = reference_pixels(packets.reference);
    NativeWindowPresentReceipt receipt{};
    assert(!presenter->present(mixed, &receipt, &present_error));
    assert(present_error.kind == NativeWindowSwapchainErrorKind::InvalidInput);

    NativeWindowPresentRequest direct_present{};
    direct_present.image = image;
    direct_present.damage_rects = full;
    direct_present.frame_id = direct_surface.frame_id;
    direct_present.ticket_id = 401U;
    direct_present.command_checksum = direct_surface.content_checksum;
    direct_present.command_count = packets.cold.header.command_count;
    direct_present.flags = kNativeWindowPresentFullRedraw;
    direct_present.shader_surface = direct_surface;
    assert(presenter->present(direct_present, &receipt, &present_error));
    assert(receipt.status == NativeWindowPresentStatus::Presented);
    assert(receipt.signal_fence_value != 0U);
    assert(presenter->retire_completed(
        receipt.signal_fence_value, &present_error));
    window->pump_events();

    assert(presenter->acquire(402U, &image, &acquire_status, &present_error));
    assert(acquire_status == NativeWindowAcquireStatus::Acquired);
    NativeShaderSurfaceView stale_surface = direct_surface;
    stale_surface.runtime_generation += 1U;
    NativeWindowPresentRequest stale_present = direct_present;
    stale_present.image = image;
    stale_present.ticket_id = 402U;
    stale_present.shader_surface = stale_surface;
    assert(!presenter->present(stale_present, &receipt, &present_error));
    assert(present_error.kind == NativeWindowSwapchainErrorKind::StaleGeneration);

    direct_present.image = image;
    direct_present.ticket_id = 402U;
    assert(presenter->present(direct_present, &receipt, &present_error));
    assert(receipt.status == NativeWindowPresentStatus::Presented);
    assert(presenter->retire_completed(
        receipt.signal_fence_value, &present_error));
    window->pump_events();

    const NativeWindowSwapchainSnapshot present_snapshot = presenter->snapshot();
    assert(present_snapshot.presented_frames >= 2U);
    assert(present_snapshot.stale_rejections >= 1U);
    assert(present_snapshot.in_flight_frame_count == 0U);

    ShaderReadback cold_readback;
    assert(executor->execute(
        packets.cold, packets.atlas, &cold_readback, &error));
    assert(readbacks_equal(cold_readback, packets.reference));

    NativeShaderSurfaceView after_readback_surface;
    assert(executor->export_surface(&after_readback_surface, &error));
    assert(after_readback_surface.output_generation ==
        direct_surface.output_generation);
    assert(after_readback_surface.native_resource ==
        direct_surface.native_resource);

    owner->shutdown();
    owner.reset();

    ShaderReadback hot_readback;
    assert(executor->execute(
        packets.hot, packets.atlas, &hot_readback, &error));
    assert(readbacks_equal(hot_readback, packets.reference));

    snapshot = executor->snapshot();
    assert(snapshot.configured == 1U);
    assert(snapshot.executions == 3U);
    assert(snapshot.readbacks == 2U);
    assert(snapshot.atlas_upload_batches == 1U);
    assert(snapshot.atlas_reuses == 2U);
    assert(snapshot.persistent_atlas_bytes == 49'152U);
    assert(snapshot.output_surface_bytes == 921'600U);
    assert(snapshot.last_readback_checksum == packets.reference.checksum);
    assert((snapshot.capability_flags &
        kNativeShaderExecutionRetainedContext) != 0U);
    assert((snapshot.capability_flags &
        kNativeShaderExecutionDirectSurfaceExport) != 0U);

    GpuShaderPacket corrupted(&packets.hot_resource);
    corrupted.header = packets.hot.header;
    corrupted.header.packet_checksum ^= 1U;
    ShaderReadback rejected;
    assert(!executor->execute(corrupted, packets.atlas, &rejected, &error));
    assert(error.kind == NativeShaderExecutionErrorKind::InvalidInput);

    presenter->shutdown();
    executor->shutdown();
    window.reset();
    std::cout << "real Metal integer shader execution: commands="
              << packets.hot.header.command_count
              << " fills=" << packets.hot.header.fill_instance_count
              << " glyphs=" << packets.hot.header.glyph_instance_count
              << " checksum=" << packets.reference.checksum
              << " direct_surface=PASS"
              << " lazy_readback=PASS"
              << " direct_present=PASS"
              << " mixed-source=reject"
              << " stale-generation=reject"
              << " PASS\n";
    return 0;
}
