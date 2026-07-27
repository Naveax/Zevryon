#include "native_vulkan_wsi.hpp"
#include "native_vulkan_wsi_test_window.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace zevryon::text;
constexpr std::uint64_t kOffset = 1469598103934665603ULL;
constexpr std::uint64_t kPrime = 1099511628211ULL;
void mix(std::uint64_t* hash, std::uint64_t value) {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        *hash ^= (value >> shift) & 0xFFU;
        *hash *= kPrime;
    }
}
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
double percentile(const std::vector<double>& samples, double value) {
    const std::size_t index = static_cast<std::size_t>(
        value * static_cast<double>(samples.size() - 1U));
    return samples[index];
}
}

int main(int argc, char** argv) {
    using namespace zevryon::text;
    std::uint32_t iterations = 256U;
    if (argc > 1) {
        iterations = static_cast<std::uint32_t>(std::stoul(argv[1]));
    }
    const NativeWindowSystem system = selected_system();
    test::NativeVulkanTestWindow window;
    std::string window_error;
    if (!window.create(system, 640U, 360U, &window_error)) {
        std::cerr << window_error << '\n';
        return 1;
    }
    NativeGpuSdkConfig owner_config;
    owner_config.api_kind = NativeGpuApiKind::Vulkan;
    owner_config.allow_software_device = 1U;
    owner_config.device_generation = 401U;
    owner_config.runtime_generation = 409U;
    owner_config.window = window.handle();
    owner_config.limits.maximum_swapchain_images = 4U;
    owner_config.limits.maximum_frames_in_flight = 2U;
    owner_config.limits.maximum_device_local_bytes = 128U * 1024U * 1024U;
    auto owner = make_vulkan_wsi_native_gpu_sdk_api();
    NativeGpuSdkError sdk_error;
    if (owner == nullptr || !owner->initialize(owner_config, &sdk_error)) {
        std::cerr << sdk_error.message << '\n';
        return 2;
    }
    NativeGpuSdkContextHandle context;
    if (!owner->export_context(&context, &sdk_error)) {
        return 3;
    }
    NativeWindowSwapchainConfig config;
    config.context = context;
    config.window = window.handle();
    config.surface.surface_id = 419U;
    config.surface.generation_id = 421U;
    config.surface.width = 640U;
    config.surface.height = 360U;
    config.surface.format = GpuSurfaceFormat::Bgra8Unorm;
    config.swapchain_generation = 431U;
    config.present_mode = NativePresentMode::Fifo;
    config.image_count = 3U;
    config.flags = kNativeWindowSwapchainAllowPartialPresent |
        kNativeWindowSwapchainRequireNativeContext;
    config.limits.maximum_image_count = 4U;
    config.limits.maximum_frames_in_flight = 2U;
    config.limits.maximum_damage_rects = 4U;
    config.limits.maximum_width = 4096U;
    config.limits.maximum_height = 4096U;
    config.limits.maximum_surface_bytes = 128U * 1024U * 1024U;
    config.limits.maximum_in_flight_bytes = 32U * 1024U * 1024U;
    auto presenter = make_vulkan_native_window_swapchain_api();
    NativeWindowSwapchainError error;
    if (presenter == nullptr || !presenter->configure(config, &error)) {
        std::cerr << error.message << '\n';
        return 4;
    }
    owner->shutdown();
    owner.reset();

    const std::array<NativeDamageRect, 4U> damage{{
        {0, 0, 160U, 180U}, {160, 0, 160U, 180U},
        {320, 180, 160U, 180U}, {480, 180, 160U, 180U}}};
    std::vector<double> samples;
    samples.reserve(iterations);
    std::uint64_t checksum = kOffset;
    std::uint32_t presented = 0U;
    std::uint32_t suboptimal = 0U;
    for (std::uint32_t index = 0U; index < iterations; ++index) {
        NativeWindowSwapchainImage image;
        NativeWindowAcquireStatus status;
        const auto start = std::chrono::steady_clock::now();
        if (!presenter->acquire(index + 1U, &image, &status, &error) ||
            (status != NativeWindowAcquireStatus::Acquired &&
             status != NativeWindowAcquireStatus::Suboptimal)) {
            std::cerr << error.message << '\n';
            return 5;
        }
        NativeWindowPresentRequest request;
        request.image = image;
        request.damage_rects = damage;
        request.frame_id = index + 1U;
        request.ticket_id = index + 1U;
        request.command_checksum = 0xCAFE0000ULL + index;
        request.command_count = 80U;
        NativeWindowPresentReceipt receipt;
        if (!presenter->present(request, &receipt, &error) ||
            !presenter->retire_completed(receipt.signal_fence_value, &error)) {
            std::cerr << error.message << '\n';
            return 6;
        }
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(end - start).count());
        if (receipt.status == NativeWindowPresentStatus::Presented) {
            ++presented;
        } else if (receipt.status == NativeWindowPresentStatus::Suboptimal) {
            ++suboptimal;
        } else {
            return 7;
        }
        mix(&checksum, index + 1U);
        mix(&checksum, receipt.signal_fence_value);
        mix(&checksum, request.command_checksum);
        mix(&checksum, static_cast<std::uint64_t>(receipt.status));
    }
    std::sort(samples.begin(), samples.end());
    const auto snapshot = presenter->snapshot();
    std::cout << std::fixed << std::setprecision(6)
              << "iterations=" << iterations << '\n'
              << "samples=" << samples.size() << '\n'
              << "presented=" << presented << '\n'
              << "suboptimal=" << suboptimal << '\n'
              << "surface_bytes=" << snapshot.current_surface_bytes << '\n'
              << "peak_in_flight_bytes=" << snapshot.peak_in_flight_bytes << '\n'
              << "p50_ms=" << percentile(samples, 0.50) << '\n'
              << "p95_ms=" << percentile(samples, 0.95) << '\n'
              << "p99_ms=" << percentile(samples, 0.99) << '\n'
              << "max_ms=" << samples.back() << '\n'
              << "checksum=" << checksum << '\n'
              << "software_device="
              << (((context.flags & kNativeGpuSdkContextSoftwareDevice) != 0U) ? 1 : 0)
              << '\n';
    return 0;
}
