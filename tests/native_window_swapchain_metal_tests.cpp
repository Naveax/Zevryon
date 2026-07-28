#include "native_metal_window.hpp"
#include "native_metal_window_test_window.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>

namespace {

using namespace zevryon::text;

GpuSurfaceDescriptor make_surface(
    std::uint64_t generation,
    std::uint32_t width,
    std::uint32_t height) {
    GpuSurfaceDescriptor surface;
    surface.surface_id = 0x4D4554414CULL;
    surface.generation_id = generation;
    surface.width = width;
    surface.height = height;
    surface.format = GpuSurfaceFormat::Bgra8Unorm;
    return surface;
}

NativeGpuSdkConfig make_owner_config(
    const NativeWindowSurfaceHandle& window) {
    NativeGpuSdkConfig config;
    config.api_kind = NativeGpuApiKind::Metal;
    config.require_real_device = 1U;
    config.allow_software_device = 0U;
    config.device_generation = 0x4D4554414C01ULL;
    config.runtime_generation = 0x4D4554414C02ULL;
    config.limits = default_native_gpu_sdk_limits(NativeGpuApiKind::Metal);
    config.window = window;
    return config;
}

NativeWindowSwapchainConfig make_swapchain_config(
    const NativeGpuSdkContextHandle& context,
    const NativeWindowSurfaceHandle& window,
    const GpuSurfaceDescriptor& surface,
    std::uint64_t generation) {
    NativeWindowSwapchainConfig config;
    config.context = context;
    config.window = window;
    config.surface = surface;
    config.limits = default_native_window_swapchain_limits(
        NativeGpuApiKind::Metal,
        NativeWindowSystem::CocoaLayer);
    config.limits.maximum_image_count = 3U;
    config.limits.maximum_frames_in_flight = 2U;
    config.limits.maximum_damage_rects = 64U;
    config.swapchain_generation = generation;
    config.present_mode = NativePresentMode::Fifo;
    config.image_count = 3U;
    config.flags = kNativeWindowSwapchainRequireNativeContext;
    return config;
}

NativeWindowPresentReceipt present_full(
    NativeWindowSwapchainApi* presenter,
    const NativeWindowSwapchainImage& image,
    std::uint64_t frame) {
    NativeWindowPresentRequest request;
    request.image = image;
    request.frame_id = frame;
    request.ticket_id = frame + 1000U;
    request.command_checksum = 0x9E3779B97F4A7C15ULL ^ frame;
    request.command_count = 1U;
    request.flags = kNativeWindowPresentFullRedraw;
    NativeWindowPresentReceipt receipt;
    NativeWindowSwapchainError error;
    assert(presenter->present(request, &receipt, &error));
    assert(error.kind == NativeWindowSwapchainErrorKind::None);
    assert(receipt.status == NativeWindowPresentStatus::Presented);
    assert(receipt.signal_fence_value != 0U);
    return receipt;
}

} // namespace

int main() {
    using namespace zevryon::text;

    assert(native_metal_window_build_has_backend(
        NativeWindowSystem::CocoaLayer));
    assert(native_window_swapchain_build_has_backend(
        NativeGpuApiKind::Metal,
        NativeWindowSystem::CocoaLayer));

    auto host = test::make_metal_window_test_host(640U, 360U);
    assert(host != nullptr);
    const NativeWindowSurfaceHandle window = host->handle();
    assert(window.system == NativeWindowSystem::CocoaLayer);
    assert(window.window_or_layer != 0U);

    auto owner = make_metal_window_native_gpu_sdk_api();
    auto presenter = make_metal_native_window_swapchain_api();
    assert(owner != nullptr);
    assert(presenter != nullptr);

    NativeGpuSdkError owner_error;
    const NativeGpuSdkConfig owner_config = make_owner_config(window);
    assert(owner->initialize(owner_config, &owner_error));
    NativeGpuSdkContextHandle context;
    assert(owner->export_context(&context, &owner_error));
    assert(context.api_kind == NativeGpuApiKind::Metal);
    assert(context.device != 0U);
    assert(context.graphics_queue == context.present_queue);

    const GpuSurfaceDescriptor surface = make_surface(1U, 640U, 360U);
    NativeWindowSwapchainConfig config =
        make_swapchain_config(context, window, surface, 1U);
    NativeWindowSwapchainError error;
    assert(presenter->configure(config, &error));

    // Prove the presenter retained the exact Metal graph after the Z2F-8A owner exits.
    owner->shutdown();

    for (std::uint64_t frame = 1U; frame <= 16U; ++frame) {
        NativeWindowSwapchainImage image;
        NativeWindowAcquireStatus status{};
        assert(presenter->acquire(frame, &image, &status, &error));
        assert(status == NativeWindowAcquireStatus::Acquired);
        const NativeWindowPresentReceipt receipt =
            present_full(presenter.get(), image, frame);
        assert(presenter->retire_completed(
            receipt.signal_fence_value,
            &error));
    }

    // A drawable can be released without presentation when no damage exists.
    NativeWindowSwapchainImage skipped_image;
    NativeWindowAcquireStatus skipped_status{};
    assert(presenter->acquire(100U, &skipped_image, &skipped_status, &error));
    assert(skipped_status == NativeWindowAcquireStatus::Acquired);
    NativeWindowPresentRequest skipped_request;
    skipped_request.image = skipped_image;
    skipped_request.frame_id = 100U;
    skipped_request.ticket_id = 1100U;
    NativeWindowPresentReceipt skipped_receipt;
    assert(presenter->present(
        skipped_request,
        &skipped_receipt,
        &error));
    assert(skipped_receipt.status ==
           NativeWindowPresentStatus::SkippedNoDamage);

    // Two frames in flight are permitted; a third acquire is backpressured.
    std::array<NativeWindowPresentReceipt, 2U> in_flight{};
    for (std::size_t index = 0U; index < in_flight.size(); ++index) {
        NativeWindowSwapchainImage image;
        NativeWindowAcquireStatus status{};
        assert(presenter->acquire(200U + static_cast<std::uint64_t>(index), &image, &status, &error));
        assert(status == NativeWindowAcquireStatus::Acquired);
        in_flight[index] = present_full(
            presenter.get(),
            image,
            200U + static_cast<std::uint64_t>(index));
    }
    NativeWindowSwapchainImage blocked_image;
    NativeWindowAcquireStatus blocked_status{};
    assert(presenter->acquire(
        299U,
        &blocked_image,
        &blocked_status,
        &error));
    assert(blocked_status == NativeWindowAcquireStatus::NotReady);
    assert(presenter->retire_completed(
        in_flight.back().signal_fence_value,
        &error));

    // Acquire from generation one, recreate, then prove the old drawable is stale.
    NativeWindowSwapchainImage stale_image;
    NativeWindowAcquireStatus stale_status{};
    assert(presenter->acquire(300U, &stale_image, &stale_status, &error));
    assert(stale_status == NativeWindowAcquireStatus::Acquired);

    const GpuSurfaceDescriptor resized = make_surface(2U, 800U, 450U);
    assert(host->resize(800U, 450U));
    assert(presenter->request_resize(resized, &error));
    NativeWindowSwapchainConfig resized_config =
        make_swapchain_config(context, window, resized, 2U);
    assert(presenter->recreate(resized_config, &error));

    NativeWindowPresentRequest stale_request;
    stale_request.image = stale_image;
    stale_request.frame_id = 301U;
    stale_request.ticket_id = 1301U;
    stale_request.command_checksum = 1U;
    stale_request.command_count = 1U;
    stale_request.flags = kNativeWindowPresentFullRedraw;
    NativeWindowPresentReceipt stale_receipt;
    assert(!presenter->present(
        stale_request,
        &stale_receipt,
        &error));
    assert(error.kind == NativeWindowSwapchainErrorKind::StaleGeneration);

    NativeWindowSwapchainImage resized_image;
    NativeWindowAcquireStatus resized_status{};
    assert(presenter->acquire(
        400U,
        &resized_image,
        &resized_status,
        &error));
    assert(resized_status == NativeWindowAcquireStatus::Acquired);
    const NativeWindowPresentReceipt resized_receipt =
        present_full(presenter.get(), resized_image, 400U);
    assert(presenter->retire_completed(
        resized_receipt.signal_fence_value,
        &error));

    const NativeWindowSwapchainSnapshot snapshot = presenter->snapshot();
    assert(snapshot.configured != 0U);
    assert(snapshot.config.context.device == context.device);
    assert(snapshot.config.context.graphics_queue == context.graphics_queue);
    assert(snapshot.config.surface == resized);
    assert(snapshot.config.swapchain_generation == 2U);
    assert(snapshot.recreations == 1U);
    assert(snapshot.presented_frames == 19U);
    assert(snapshot.skipped_frames == 1U);
    assert(snapshot.stale_rejections >= 1U);
    assert(snapshot.current_surface_bytes == 4'320'000U);
    assert(snapshot.peak_in_flight_bytes >= 1'843'200U);

    presenter->shutdown();
    return 0;
}
