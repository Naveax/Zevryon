#include "native_shader_execution.hpp"
#include "native_vulkan_wsi.hpp"
#include "native_vulkan_wsi_test_window.hpp"
#include "shader_draw_packet_fixture.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
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

NativeWindowSystem selected_system() {
#if defined(_WIN32)
    return NativeWindowSystem::Win32;
#else
    const char* value = std::getenv("ZEVRYON_VULKAN_WSI_SYSTEM");
    return value != nullptr && std::string(value) == "wayland"
        ? NativeWindowSystem::Wayland
        : NativeWindowSystem::Xcb;
#endif
}

bool build_packets(PacketSet* output) {
    if (output == nullptr) {
        return false;
    }
    ShaderPacketFixture fixture = make_shader_packet_fixture();
    ShaderPacketError packet_error;
    if (!compile_gpu_shader_packet(fixture.input(), &output->cold, &packet_error) ||
        !output->atlas.apply_packet_uploads(output->cold, &packet_error) ||
        !execute_shader_packet_reference(
            output->cold, output->atlas, &output->reference, &packet_error)) {
        std::cerr << "packet preparation failed: " << packet_error.message << '\n';
        return false;
    }
    fixture.uploads.clear();
    fixture.payload.clear();
    if (!compile_gpu_shader_packet(
            fixture.input(2U, 7U), &output->hot, &packet_error)) {
        std::cerr << "hot packet preparation failed: "
                  << packet_error.message << '\n';
        return false;
    }
    return true;
}

NativeGpuSdkConfig owner_config(
    const NativeWindowSurfaceHandle& window) {
    NativeGpuSdkConfig config{};
    config.api_kind = NativeGpuApiKind::Vulkan;
    config.allow_software_device = 1U;
    config.require_real_device = 0U;
    config.enable_validation = 0U;
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
    const NativeWindowSurfaceHandle& window,
    std::uint32_t width,
    std::uint32_t height) {
    NativeWindowSwapchainConfig config{};
    config.context = context;
    config.window = window;
    config.surface.surface_id = 317U;
    config.surface.generation_id = 319U;
    config.surface.width = width;
    config.surface.height = height;
    config.surface.format = GpuSurfaceFormat::Bgra8Unorm;
    config.surface.premultiplied_alpha = 1U;
    config.swapchain_generation = 323U;
    config.present_mode = NativePresentMode::Fifo;
    config.image_count = 3U;
    config.flags = kNativeWindowSwapchainAllowMailbox |
        kNativeWindowSwapchainAllowImmediate |
        kNativeWindowSwapchainAllowPartialPresent |
        kNativeWindowSwapchainRequireNativeContext;
    config.limits.maximum_image_count = 4U;
    config.limits.maximum_frames_in_flight = 2U;
    config.limits.maximum_damage_rects = 8U;
    config.limits.maximum_width = 4096U;
    config.limits.maximum_height = 4096U;
    config.limits.maximum_surface_bytes = 128U * 1024U * 1024U;
    config.limits.maximum_in_flight_bytes = 32U * 1024U * 1024U;
    return config;
}

NativeWindowPixelBufferView reference_pixels(
    const ShaderReadback& reference) {
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

NativeWindowSwapchainImage acquire_image(
    NativeWindowSwapchainApi* presenter,
    std::uint64_t ticket,
    NativeWindowSwapchainError* error) {
    NativeWindowSwapchainImage image{};
    NativeWindowAcquireStatus status = NativeWindowAcquireStatus::NotReady;
    assert(presenter->acquire(ticket, &image, &status, error));
    assert(status == NativeWindowAcquireStatus::Acquired ||
           status == NativeWindowAcquireStatus::Suboptimal);
    return image;
}

NativeWindowPresentReceipt present_surface(
    NativeWindowSwapchainApi* presenter,
    const NativeWindowSwapchainImage& image,
    std::uint64_t ticket,
    const GpuShaderPacket& packet,
    const NativeShaderSurfaceView& surface,
    NativeWindowSwapchainError* error) {
    const std::array<NativeDamageRect, 1U> full{{{
        0,
        0,
        packet.header.surface_width,
        packet.header.surface_height}}};
    NativeWindowPresentRequest request{};
    request.image = image;
    request.damage_rects = full;
    request.frame_id = surface.frame_id;
    request.ticket_id = ticket;
    request.command_checksum = surface.content_checksum;
    request.command_count = packet.header.command_count;
    request.flags = kNativeWindowPresentFullRedraw;
    request.shader_surface = surface;
    NativeWindowPresentReceipt receipt{};
    assert(presenter->present(request, &receipt, error));
    assert(receipt.status == NativeWindowPresentStatus::Presented ||
           receipt.status == NativeWindowPresentStatus::Suboptimal);
    assert(receipt.signal_fence_value != 0U);
    assert(presenter->retire_completed(receipt.signal_fence_value, error));
    return receipt;
}

} // namespace

int main() {
    const NativeWindowSystem system = selected_system();
    assert(native_vulkan_wsi_build_has_window_system(system));
    assert(native_shader_execution_build_has_backend(NativeGpuApiKind::Vulkan));

    PacketSet packets;
    assert(build_packets(&packets));
    assert(packets.cold.header.surface_width == 640U);
    assert(packets.cold.header.surface_height == 360U);

    NativeVulkanTestWindow window;
    std::string window_error;
    assert(window.create(system, 640U, 360U, &window_error));

    auto owner = make_vulkan_wsi_native_gpu_sdk_api();
    assert(owner != nullptr);
    NativeGpuSdkError sdk_error;
    assert(owner->initialize(owner_config(window.handle()), &sdk_error));
    NativeGpuSdkContextHandle context{};
    assert(owner->export_context(&context, &sdk_error));

    auto executor = make_vulkan_native_shader_executor();
    assert(executor != nullptr);
    NativeShaderExecutionConfig execution_config{};
    execution_config.context = context;
    execution_config.limits = default_native_shader_execution_limits(
        NativeGpuApiKind::Vulkan);
    execution_config.executor_generation = 331U;
    NativeShaderExecutionError execution_error;
    assert(executor->configure(execution_config, &execution_error));

    auto presenter = make_vulkan_native_window_swapchain_api();
    assert(presenter != nullptr);
    NativeWindowSwapchainError present_error;
    assert(presenter->configure(
        swapchain_config(context, window.handle(), 640U, 360U),
        &present_error));

    // Both consumers retain the same native Vulkan graph. The owner may close
    // before execution/presentation without invalidating either lease.
    owner->shutdown();
    owner.reset();

    assert(executor->execute(
        packets.cold, packets.atlas, nullptr, &execution_error));
    NativeShaderSurfaceView cold_surface{};
    assert(executor->export_surface(&cold_surface, &execution_error));
    assert(cold_surface.frame_id == packets.cold.header.frame_id);
    assert(cold_surface.content_checksum == packets.cold.header.packet_checksum);

    NativeWindowSwapchainImage image =
        acquire_image(presenter.get(), 401U, &present_error);
    const std::array<NativeDamageRect, 1U> full{{{0, 0, 640U, 360U}}};
    NativeWindowPresentRequest mixed{};
    mixed.image = image;
    mixed.damage_rects = full;
    mixed.frame_id = cold_surface.frame_id;
    mixed.ticket_id = 401U;
    mixed.command_checksum = cold_surface.content_checksum;
    mixed.command_count = packets.cold.header.command_count;
    mixed.flags = kNativeWindowPresentFullRedraw;
    mixed.shader_surface = cold_surface;
    mixed.pixel_buffer = reference_pixels(packets.reference);
    NativeWindowPresentReceipt receipt{};
    assert(!presenter->present(mixed, &receipt, &present_error));
    assert(present_error.kind == NativeWindowSwapchainErrorKind::InvalidInput);

    present_surface(
        presenter.get(), image, 401U, packets.cold, cold_surface,
        &present_error);
    assert(window.pump(&window_error));

    auto snapshot = executor->snapshot();
    assert(snapshot.executions == 1U);
    assert(snapshot.readbacks == 0U);
    assert((snapshot.capability_flags &
        kNativeShaderExecutionDirectSurfaceExport) != 0U);

    assert(executor->execute(
        packets.hot, packets.atlas, nullptr, &execution_error));
    NativeShaderSurfaceView hot_surface{};
    assert(executor->export_surface(&hot_surface, &execution_error));
    assert(hot_surface.frame_id == packets.hot.header.frame_id);
    assert(hot_surface.content_checksum == packets.hot.header.packet_checksum);

    image = acquire_image(presenter.get(), 402U, &present_error);
    NativeShaderSurfaceView stale = hot_surface;
    stale.runtime_generation += 1U;
    NativeWindowPresentRequest stale_request{};
    stale_request.image = image;
    stale_request.damage_rects = full;
    stale_request.frame_id = hot_surface.frame_id;
    stale_request.ticket_id = 402U;
    stale_request.command_checksum = hot_surface.content_checksum;
    stale_request.command_count = packets.hot.header.command_count;
    stale_request.flags = kNativeWindowPresentFullRedraw;
    stale_request.shader_surface = stale;
    assert(!presenter->present(stale_request, &receipt, &present_error));
    assert(present_error.kind == NativeWindowSwapchainErrorKind::StaleGeneration);

    present_surface(
        presenter.get(), image, 402U, packets.hot, hot_surface,
        &present_error);
    assert(window.pump(&window_error));

    snapshot = executor->snapshot();
    assert(snapshot.executions == 2U);
    assert(snapshot.readbacks == 0U);
    const auto present_snapshot = presenter->snapshot();
    assert(present_snapshot.presented_frames >= 2U);
    assert(present_snapshot.stale_rejections >= 1U);
    assert(present_snapshot.in_flight_frame_count == 0U);

    presenter->shutdown();
    executor->shutdown();
    window.destroy();
    std::cout << "real Vulkan direct shader-surface presentation: "
              << "readbacks=0 mixed-source=reject stale-generation=reject PASS\n";
    return 0;
}