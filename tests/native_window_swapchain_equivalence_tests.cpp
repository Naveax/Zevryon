#include "native_window_swapchain.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {
using namespace zevryon::text;

NativeWindowSystem system_for(NativeGpuApiKind kind) {
    switch (kind) {
        case NativeGpuApiKind::Vulkan: return NativeWindowSystem::Wayland;
        case NativeGpuApiKind::Metal: return NativeWindowSystem::CocoaLayer;
        case NativeGpuApiKind::Direct3D12: return NativeWindowSystem::Win32;
        case NativeGpuApiKind::ReferenceCpu: break;
    }
    return NativeWindowSystem::Headless;
}

NativeWindowSwapchainConfig config_for(
    NativeGpuApiKind kind,
    std::uint32_t mask,
    std::uint64_t surface_generation = 11U,
    std::uint64_t swapchain_generation = 13U) {
    const NativeWindowSystem system = system_for(kind);
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
    config.context.device = 15U;
    config.context.graphics_queue = 17U;
    config.context.present_queue = 17U;
    config.window.generation = 19U;
    config.window.display_or_instance = 21U;
    config.window.window_or_layer = 23U;
    config.window.system = system;
    config.surface.surface_id = 29U;
    config.surface.generation_id = surface_generation;
    config.surface.width = 64U + (mask % 8U) * 8U;
    config.surface.height = 48U + ((mask >> 3U) % 8U) * 8U;
    config.surface.format =
        (mask & 1U) == 0U
        ? GpuSurfaceFormat::Bgra8Unorm
        : GpuSurfaceFormat::Rgba8Unorm;
    config.limits = default_native_window_swapchain_limits(kind, system);
    config.swapchain_generation = swapchain_generation;
    config.present_mode = NativePresentMode::Fifo;
    config.image_count = 3U;
    config.flags =
        kNativeWindowSwapchainAllowPartialPresent |
        kNativeWindowSwapchainRequireNativeContext;
    return config;
}

std::array<NativeDamageRect, 2U> damage_for(
    const GpuSurfaceDescriptor& surface,
    std::uint32_t mask) {
    const std::uint64_t width = std::max<std::uint32_t>(1U, surface.width / 4U);
    const std::uint64_t height = std::max<std::uint32_t>(1U, surface.height / 4U);
    return {{
        {0, 0, width, height},
        {
            static_cast<std::int64_t>((mask % 2U) * width),
            static_cast<std::int64_t>(((mask >> 1U) % 2U) * height),
            width,
            height,
        },
    }};
}

NativeWindowPresentRequest request_for(
    const NativeWindowSwapchainImage& image,
    std::span<const NativeDamageRect> damage,
    std::uint32_t mask,
    std::uint32_t variant) {
    NativeWindowPresentRequest request;
    request.image = image;
    request.damage_rects = damage;
    request.frame_id = 1000U + mask;
    request.ticket_id = 2000U + variant;
    request.wait_fence_value = mask & 3U;
    request.command_checksum =
        (static_cast<std::uint64_t>(mask) << 32U) | variant;
    request.command_count = 5U + (mask % 7U);
    return request;
}

void run_case(
    NativeGpuApiKind kind,
    std::uint32_t mask,
    std::uint32_t variant) {
    const NativeWindowSystem system = system_for(kind);
    ReferenceNativeWindowSwapchainApi api(
        default_native_window_swapchain_capabilities(kind, system));
    NativeWindowSwapchainError error;
    NativeWindowSwapchainConfig config = config_for(kind, mask);
    assert(api.configure(config, &error));

    NativeWindowSwapchainImage image;
    NativeWindowAcquireStatus acquire = NativeWindowAcquireStatus::NotReady;
    if (variant == 6U) {
        api.set_next_acquire_status(NativeWindowAcquireStatus::OutOfDate);
    } else if (variant == 7U) {
        api.set_next_acquire_status(NativeWindowAcquireStatus::Occluded);
    } else if (variant == 8U) {
        api.set_next_acquire_status(NativeWindowAcquireStatus::DeviceLost);
    }
    assert(api.acquire(mask + 1U, &image, &acquire, &error));
    if (variant == 6U) {
        assert(acquire == NativeWindowAcquireStatus::OutOfDate);
        return;
    }
    if (variant == 7U) {
        assert(acquire == NativeWindowAcquireStatus::Occluded);
        return;
    }
    if (variant == 8U) {
        assert(acquire == NativeWindowAcquireStatus::DeviceLost);
        return;
    }
    assert(acquire == NativeWindowAcquireStatus::Acquired);

    auto damage = damage_for(config.surface, mask);
    std::span<const NativeDamageRect> damage_span = damage;
    if (variant == 5U) {
        damage_span = {};
    } else if (variant == 4U) {
        damage[0].inline_start = -1;
    }
    NativeWindowPresentRequest request =
        request_for(image, damage_span, mask, variant);
    if (variant == 1U) {
        request.image.swapchain_generation += 1U;
    } else if (variant == 2U) {
        request.image.image.image.device_generation += 1U;
    } else if (variant == 3U) {
        request.image.image.driver_generation += 1U;
    } else if (variant == 9U) {
        api.set_next_present_status(NativeWindowPresentStatus::Suboptimal);
    }

    NativeWindowPresentReceipt receipt;
    const bool success = api.present(request, &receipt, &error);
    if (variant >= 1U && variant <= 3U) {
        assert(!success);
        assert(error.kind == NativeWindowSwapchainErrorKind::StaleGeneration);
        return;
    }
    if (variant == 4U) {
        assert(!success);
        assert(error.kind == NativeWindowSwapchainErrorKind::InvalidInput);
        return;
    }
    assert(success);
    if (variant == 5U) {
        assert(receipt.status == NativeWindowPresentStatus::SkippedNoDamage);
        return;
    }
    if (variant == 9U) {
        assert(receipt.status == NativeWindowPresentStatus::Suboptimal);
    } else {
        assert(receipt.status == NativeWindowPresentStatus::Presented);
    }

    if (variant == 10U) {
        assert(api.retire_completed(receipt.signal_fence_value, &error));
        GpuSurfaceDescriptor resized = config.surface;
        resized.generation_id += 1U;
        resized.width += 16U;
        resized.height += 16U;
        assert(api.request_resize(resized, &error));
        NativeWindowSwapchainConfig replacement =
            config_for(kind, mask, resized.generation_id,
                       config.swapchain_generation + 1U);
        replacement.surface.width = resized.width;
        replacement.surface.height = resized.height;
        assert(api.recreate(replacement, &error));
        const NativeWindowSwapchainSnapshot snapshot = api.snapshot();
        assert(snapshot.recreations == 1U);
        assert(snapshot.config.surface == resized);
        return;
    }
    if (variant == 11U) {
        assert(!api.retire_completed(receipt.signal_fence_value + 1U, &error));
        assert(error.kind == NativeWindowSwapchainErrorKind::FenceRegression);
        return;
    }
    assert(api.retire_completed(receipt.signal_fence_value, &error));
}

} // namespace

int main() {
    std::uint64_t cases = 0U;
    for (NativeGpuApiKind kind : {
             NativeGpuApiKind::Vulkan,
             NativeGpuApiKind::Metal,
             NativeGpuApiKind::Direct3D12}) {
        for (std::uint32_t mask = 0U; mask < 256U; ++mask) {
            for (std::uint32_t variant = 0U; variant < 12U; ++variant) {
                run_case(kind, mask, variant);
                ++cases;
            }
        }
    }
    assert(cases == 9216U);
    std::cout << "native window swapchain oracle: "
              << cases << "/" << cases << " PASS\n";
    return 0;
}
