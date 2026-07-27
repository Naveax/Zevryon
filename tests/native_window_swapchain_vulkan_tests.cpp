#include "native_vulkan_wsi.hpp"
#include "native_vulkan_wsi_test_window.hpp"

#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {
using namespace zevryon::text;

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

NativeGpuSdkConfig owner_config(
    const NativeWindowSurfaceHandle& window) {
    NativeGpuSdkConfig config;
    config.api_kind = NativeGpuApiKind::Vulkan;
    config.allow_software_device = 1U;
    config.require_real_device = 0U;
    config.device_generation = 101U;
    config.runtime_generation = 103U;
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
    std::uint32_t height,
    std::uint64_t surface_generation,
    std::uint64_t swapchain_generation) {
    NativeWindowSwapchainConfig config;
    config.context = context;
    config.window = window;
    config.surface.surface_id = 107U;
    config.surface.generation_id = surface_generation;
    config.surface.width = width;
    config.surface.height = height;
    config.surface.format = GpuSurfaceFormat::Bgra8Unorm;
    config.swapchain_generation = swapchain_generation;
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

void present_one(
    NativeWindowSwapchainApi* presenter,
    std::uint64_t frame,
    std::uint32_t width,
    std::uint32_t height) {
    NativeWindowSwapchainError error;
    NativeWindowSwapchainImage image;
    NativeWindowAcquireStatus acquire_status;
    assert(presenter->acquire(frame, &image, &acquire_status, &error));
    assert(acquire_status == NativeWindowAcquireStatus::Acquired ||
           acquire_status == NativeWindowAcquireStatus::Suboptimal);
    const std::array<NativeDamageRect, 2U> damage{{
        {0, 0, width / 2U, height},
        {static_cast<std::int64_t>(width / 2U), 0, width - width / 2U, height}}};
    NativeWindowPresentRequest request;
    request.image = image;
    request.damage_rects = damage;
    request.frame_id = frame;
    request.ticket_id = frame;
    request.command_checksum = 0xABCD0000ULL + frame;
    request.command_count = 80U;
    request.flags = kNativeWindowPresentFullRedraw;
    NativeWindowPresentReceipt receipt;
    assert(presenter->present(request, &receipt, &error));
    assert(receipt.status == NativeWindowPresentStatus::Presented ||
           receipt.status == NativeWindowPresentStatus::Suboptimal);
    assert(receipt.signal_fence_value != 0U);
    assert(presenter->retire_completed(receipt.signal_fence_value, &error));
}

} // namespace

int main() {
    using namespace zevryon::text;
    const NativeWindowSystem system = selected_system();
    assert(native_vulkan_wsi_build_has_window_system(system));

    test::NativeVulkanTestWindow window;
    std::string window_error;
    assert(window.create(system, 640U, 360U, &window_error));

    auto owner = make_vulkan_wsi_native_gpu_sdk_api();
    assert(owner != nullptr);
    NativeGpuSdkError sdk_error;
    assert(owner->initialize(owner_config(window.handle()), &sdk_error));
    NativeGpuSdkContextHandle context;
    assert(owner->export_context(&context, &sdk_error));
    assert((context.flags & kNativeGpuSdkContextDeviceValid) != 0U);
    assert((context.flags & kNativeGpuSdkContextPresentQueueValid) != 0U);

    auto presenter = make_vulkan_native_window_swapchain_api();
    assert(presenter != nullptr);
    NativeWindowSwapchainError error;
    auto config = swapchain_config(
        context, window.handle(), 640U, 360U, 109U, 113U);
    assert(presenter->configure(config, &error));

    // Presenter retains the exact Vulkan device graph; owner shutdown must not
    // invalidate the configured swapchain.
    owner->shutdown();
    owner.reset();
    for (std::uint64_t frame = 1U; frame <= 12U; ++frame) {
        present_one(presenter.get(), frame, 640U, 360U);
        assert(window.pump(&window_error));
    }

    // Acquired images cannot escape the bounded frame ring.
    NativeWindowSwapchainImage first;
    NativeWindowSwapchainImage second;
    NativeWindowSwapchainImage third;
    NativeWindowAcquireStatus status;
    assert(presenter->acquire(20U, &first, &status, &error));
    assert(status == NativeWindowAcquireStatus::Acquired ||
           status == NativeWindowAcquireStatus::Suboptimal);
    assert(presenter->acquire(21U, &second, &status, &error));
    assert(status == NativeWindowAcquireStatus::Acquired ||
           status == NativeWindowAcquireStatus::Suboptimal);
    assert(presenter->acquire(22U, &third, &status, &error));
    assert(status == NativeWindowAcquireStatus::NotReady);

    const std::array<NativeDamageRect, 1U> full{{{0, 0, 640U, 360U}}};
    NativeWindowPresentRequest request;
    request.image = first;
    request.damage_rects = full;
    request.frame_id = 20U;
    request.ticket_id = 20U;
    request.command_checksum = 20U;
    request.command_count = 80U;
    request.flags = kNativeWindowPresentFullRedraw;
    NativeWindowPresentReceipt receipt;
    assert(presenter->present(request, &receipt, &error));
    assert(presenter->retire_completed(receipt.signal_fence_value, &error));
    request.image = second;
    request.frame_id = 21U;
    request.ticket_id = 21U;
    assert(presenter->present(request, &receipt, &error));
    assert(presenter->retire_completed(receipt.signal_fence_value, &error));

    // Recreate on a new generation and reject an old image lease.
    GpuSurfaceDescriptor resized = config.surface;
    resized.generation_id = 110U;
    assert(presenter->request_resize(resized, &error));
    auto recreated = swapchain_config(
        context, window.handle(), 640U, 360U, 110U, 114U);
    assert(presenter->recreate(recreated, &error));
    request.image = first;
    request.frame_id = 30U;
    request.ticket_id = 30U;
    assert(!presenter->present(request, &receipt, &error));
    assert(error.kind == NativeWindowSwapchainErrorKind::StaleGeneration);
    present_one(presenter.get(), 31U, 640U, 360U);

    const auto snapshot = presenter->snapshot();
    assert(snapshot.configurations == 1U);
    assert(snapshot.recreations == 1U);
    assert(snapshot.presented_frames >= 15U);
    assert(snapshot.stale_rejections >= 1U);
    assert(snapshot.in_flight_frame_count == 0U);
    presenter->shutdown();
    std::cout << "real Vulkan WSI retained-context lifecycle: PASS\n";
    return 0;
}
