#include "native_window_swapchain.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {
using namespace zevryon::text;

NativeWindowSystem window_system_for(NativeGpuApiKind kind) {
    switch (kind) {
        case NativeGpuApiKind::Vulkan: return NativeWindowSystem::Xcb;
        case NativeGpuApiKind::Metal: return NativeWindowSystem::CocoaLayer;
        case NativeGpuApiKind::Direct3D12: return NativeWindowSystem::Win32;
        case NativeGpuApiKind::ReferenceCpu: break;
    }
    return NativeWindowSystem::Headless;
}

NativeWindowSwapchainConfig make_config(
    NativeGpuApiKind kind,
    std::uint64_t surface_generation = 11U,
    std::uint64_t swapchain_generation = 17U,
    std::uint32_t width = 1280U,
    std::uint32_t height = 720U) {
    const NativeWindowSystem system = window_system_for(kind);
    NativeWindowSwapchainConfig config;
    config.context.api_kind = kind;
    config.context.flags =
        kNativeGpuSdkContextDeviceValid |
        kNativeGpuSdkContextGraphicsQueueValid |
        kNativeGpuSdkContextPresentQueueValid |
        kNativeGpuSdkContextSharedGraphicsPresentQueue;
    config.context.device_generation = 3U;
    config.context.runtime_generation = 5U;
    config.context.instance_or_factory = 7U;
    config.context.physical_device_or_adapter = 9U;
    config.context.device = 13U;
    config.context.graphics_queue = 15U;
    config.context.present_queue = 15U;
    config.window.generation = 19U;
    config.window.display_or_instance = 23U;
    config.window.window_or_layer = 29U;
    config.window.system = system;
    config.surface.surface_id = 31U;
    config.surface.generation_id = surface_generation;
    config.surface.width = width;
    config.surface.height = height;
    config.surface.format = GpuSurfaceFormat::Bgra8Unorm;
    config.limits = default_native_window_swapchain_limits(kind, system);
    config.swapchain_generation = swapchain_generation;
    config.present_mode = NativePresentMode::Fifo;
    config.image_count = 3U;
    config.flags =
        kNativeWindowSwapchainAllowPartialPresent |
        kNativeWindowSwapchainRequireNativeContext;
    return config;
}

NativeWindowPresentRequest make_present(
    const NativeWindowSwapchainImage& image,
    std::span<const NativeDamageRect> damage,
    std::uint64_t frame_id = 41U,
    std::uint64_t ticket_id = 43U) {
    NativeWindowPresentRequest request;
    request.image = image;
    request.damage_rects = damage;
    request.frame_id = frame_id;
    request.ticket_id = ticket_id;
    request.command_checksum = 47U;
    request.command_count = 5U;
    return request;
}

void basic_lifecycle() {
    const NativeGpuApiKind kind = NativeGpuApiKind::Direct3D12;
    ReferenceNativeWindowSwapchainApi api(
        default_native_window_swapchain_capabilities(
            kind, window_system_for(kind)));
    NativeWindowSwapchainError error;
    const NativeWindowSwapchainConfig config = make_config(kind);
    assert(api.configure(config, &error));

    NativeWindowSwapchainImage image;
    NativeWindowAcquireStatus acquire = NativeWindowAcquireStatus::NotReady;
    assert(api.acquire(1U, &image, &acquire, &error));
    assert(acquire == NativeWindowAcquireStatus::Acquired);
    assert((image.flags & kNativeWindowSwapchainImageAcquired) != 0U);

    const std::array<NativeDamageRect, 2U> damage{{
        {0, 0, 320U, 180U},
        {640, 360, 320U, 180U},
    }};
    NativeWindowPresentReceipt receipt;
    const NativeWindowPresentRequest request = make_present(image, damage);
    assert(api.present(request, &receipt, &error));
    assert(receipt.status == NativeWindowPresentStatus::Presented);
    assert(receipt.signal_fence_value == 1U);
    assert(receipt.damage_rect_count == damage.size());

    NativeWindowSwapchainSnapshot snapshot = api.snapshot();
    assert(snapshot.presented_frames == 1U);
    assert(snapshot.in_flight_frame_count == 1U);
    assert(snapshot.current_surface_bytes == 1280ULL * 720ULL * 4ULL * 3ULL);
    assert(snapshot.current_in_flight_bytes == 1280ULL * 720ULL * 4ULL);

    assert(api.retire_completed(receipt.signal_fence_value, &error));
    snapshot = api.snapshot();
    assert(snapshot.in_flight_frame_count == 0U);
    assert(snapshot.current_in_flight_bytes == 0U);
}

void skipped_and_suboptimal() {
    const NativeGpuApiKind kind = NativeGpuApiKind::Vulkan;
    ReferenceNativeWindowSwapchainApi api(
        default_native_window_swapchain_capabilities(
            kind, window_system_for(kind)));
    NativeWindowSwapchainError error;
    assert(api.configure(make_config(kind), &error));

    NativeWindowSwapchainImage image;
    NativeWindowAcquireStatus acquire = NativeWindowAcquireStatus::NotReady;
    api.set_next_acquire_status(NativeWindowAcquireStatus::Suboptimal);
    assert(api.acquire(1U, &image, &acquire, &error));
    assert(acquire == NativeWindowAcquireStatus::Suboptimal);
    assert((image.flags & kNativeWindowSwapchainImageSuboptimal) != 0U);

    NativeWindowPresentReceipt receipt;
    NativeWindowPresentRequest request = make_present(image, {});
    assert(api.present(request, &receipt, &error));
    assert(receipt.status == NativeWindowPresentStatus::SkippedNoDamage);
    assert(receipt.signal_fence_value == 0U);
    const NativeWindowSwapchainSnapshot snapshot = api.snapshot();
    assert(snapshot.skipped_frames == 1U);
    assert(snapshot.acquired_image_count == 0U);
}

void stale_and_damage_rejection() {
    const NativeGpuApiKind kind = NativeGpuApiKind::Vulkan;
    ReferenceNativeWindowSwapchainApi api(
        default_native_window_swapchain_capabilities(
            kind, window_system_for(kind)));
    NativeWindowSwapchainError error;
    assert(api.configure(make_config(kind), &error));

    NativeWindowSwapchainImage image;
    NativeWindowAcquireStatus acquire = NativeWindowAcquireStatus::NotReady;
    assert(api.acquire(1U, &image, &acquire, &error));
    NativeWindowSwapchainImage stale = image;
    stale.swapchain_generation += 1U;
    const std::array<NativeDamageRect, 1U> damage{{{0, 0, 16U, 16U}}};
    NativeWindowPresentReceipt receipt;
    NativeWindowPresentRequest stale_request = make_present(stale, damage);
    assert(!api.present(stale_request, &receipt, &error));
    assert(error.kind == NativeWindowSwapchainErrorKind::StaleGeneration);

    const std::array<NativeDamageRect, 1U> invalid{{
        {-1, 0, 16U, 16U},
    }};
    NativeWindowPresentRequest invalid_request = make_present(image, invalid);
    assert(!api.present(invalid_request, &receipt, &error));
    assert(error.kind == NativeWindowSwapchainErrorKind::InvalidInput);

    NativeWindowPresentRequest valid_request = make_present(image, damage);
    assert(api.present(valid_request, &receipt, &error));
    assert(api.retire_completed(receipt.signal_fence_value, &error));
}

void resize_and_recreate() {
    const NativeGpuApiKind kind = NativeGpuApiKind::Direct3D12;
    ReferenceNativeWindowSwapchainApi api(
        default_native_window_swapchain_capabilities(
            kind, window_system_for(kind)));
    NativeWindowSwapchainError error;
    NativeWindowSwapchainConfig config = make_config(kind);
    assert(api.configure(config, &error));

    NativeWindowSwapchainImage image;
    NativeWindowAcquireStatus acquire = NativeWindowAcquireStatus::NotReady;
    assert(api.acquire(1U, &image, &acquire, &error));
    const std::array<NativeDamageRect, 1U> damage{{{0, 0, 64U, 64U}}};
    NativeWindowPresentReceipt receipt;
    assert(api.present(make_present(image, damage), &receipt, &error));

    GpuSurfaceDescriptor resized = config.surface;
    resized.generation_id += 1U;
    resized.width = 1600U;
    resized.height = 900U;
    assert(api.request_resize(resized, &error));

    NativeWindowSwapchainImage blocked_image;
    assert(api.acquire(2U, &blocked_image, &acquire, &error));
    assert(acquire == NativeWindowAcquireStatus::OutOfDate);

    NativeWindowSwapchainConfig replacement =
        make_config(kind, resized.generation_id, config.swapchain_generation + 1U,
                    resized.width, resized.height);
    assert(!api.recreate(replacement, &error));
    assert(error.kind == NativeWindowSwapchainErrorKind::Backpressure);
    assert(api.retire_completed(receipt.signal_fence_value, &error));
    assert(api.recreate(replacement, &error));

    const NativeWindowSwapchainSnapshot snapshot = api.snapshot();
    assert(snapshot.recreations == 1U);
    assert(snapshot.resize_requests == 1U);
    assert(snapshot.config.surface == resized);
    assert(snapshot.out_of_date == 0U);
}

void backpressure_and_statuses() {
    const NativeGpuApiKind kind = NativeGpuApiKind::Vulkan;
    ReferenceNativeWindowSwapchainApi api(
        default_native_window_swapchain_capabilities(
            kind, window_system_for(kind)));
    NativeWindowSwapchainError error;
    assert(api.configure(make_config(kind), &error));
    const std::array<NativeDamageRect, 1U> damage{{{0, 0, 8U, 8U}}};

    std::array<NativeWindowPresentReceipt, 2U> receipts{};
    for (std::uint32_t index = 0U; index < receipts.size(); ++index) {
        NativeWindowSwapchainImage image;
        NativeWindowAcquireStatus acquire = NativeWindowAcquireStatus::NotReady;
        assert(api.acquire(index + 1U, &image, &acquire, &error));
        assert(acquire == NativeWindowAcquireStatus::Acquired);
        assert(api.present(
            make_present(image, damage, 100U + index, 200U + index),
            &receipts[index],
            &error));
    }
    NativeWindowSwapchainImage third;
    NativeWindowAcquireStatus acquire = NativeWindowAcquireStatus::Acquired;
    assert(api.acquire(3U, &third, &acquire, &error));
    assert(acquire == NativeWindowAcquireStatus::NotReady);

    api.shutdown();
    assert(api.configure(make_config(kind), &error));
    api.set_occluded(true);
    assert(api.acquire(4U, &third, &acquire, &error));
    assert(acquire == NativeWindowAcquireStatus::Occluded);
    api.set_occluded(false);
    api.set_device_lost(true);
    assert(api.acquire(5U, &third, &acquire, &error));
    assert(acquire == NativeWindowAcquireStatus::DeviceLost);
}

void invalid_modes_and_context() {
    const NativeGpuApiKind kind = NativeGpuApiKind::Direct3D12;
    ReferenceNativeWindowSwapchainApi api(
        default_native_window_swapchain_capabilities(
            kind, window_system_for(kind)));
    NativeWindowSwapchainError error;
    NativeWindowSwapchainConfig config = make_config(kind);
    config.present_mode = NativePresentMode::Mailbox;
    config.flags |= kNativeWindowSwapchainAllowMailbox;
    assert(!api.configure(config, &error));
    assert(error.kind == NativeWindowSwapchainErrorKind::UnsupportedPresentMode);

    config = make_config(kind);
    config.context.device = 0U;
    assert(!api.configure(config, &error));
    assert(error.kind == NativeWindowSwapchainErrorKind::NativeContextUnavailable);
}

void metal_is_unsupported() {
    const NativeWindowSwapchainCapabilities capabilities =
        default_native_window_swapchain_capabilities(
            NativeGpuApiKind::Metal, NativeWindowSystem::CocoaLayer);
    const NativeWindowSwapchainLimits limits =
        default_native_window_swapchain_limits(
            NativeGpuApiKind::Metal, NativeWindowSystem::CocoaLayer);
    assert(!native_window_swapchain_build_has_backend(
        NativeGpuApiKind::Metal, NativeWindowSystem::CocoaLayer));
    assert(capabilities.flags == 0U);
    assert(capabilities.maximum_image_count == 0U);
    assert(capabilities.maximum_frames_in_flight == 0U);
    assert(capabilities.maximum_surface_bytes == 0U);
    assert(limits.maximum_image_count == 0U);
    assert(limits.maximum_frames_in_flight == 0U);
    assert(limits.maximum_surface_bytes == 0U);
    assert(limits.maximum_in_flight_bytes == 0U);

    ReferenceNativeWindowSwapchainApi api(capabilities);
    NativeWindowSwapchainError error;
    assert(!api.configure(make_config(NativeGpuApiKind::Metal), &error));
    assert(error.kind == NativeWindowSwapchainErrorKind::NativeContextUnavailable);
}

} // namespace

int main() {
    basic_lifecycle();
    skipped_and_suboptimal();
    stale_and_damage_rejection();
    resize_and_recreate();
    backpressure_and_statuses();
    invalid_modes_and_context();
    metal_is_unsupported();
    std::cout << "native window swapchain tests: PASS\n";
    return 0;
}
