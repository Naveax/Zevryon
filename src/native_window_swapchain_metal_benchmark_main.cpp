#include "native_metal_window.hpp"
#include "native_metal_window_test_window.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

using namespace zevryon::text;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

template <typename T>
void hash_value(std::uint64_t* hash, const T& value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        *hash ^= bytes[index];
        *hash *= kFnvPrime;
    }
}

double percentile(std::vector<double> samples, double fraction) {
    std::sort(samples.begin(), samples.end());
    const std::size_t index = static_cast<std::size_t>(
        fraction * static_cast<double>(samples.size() - 1U));
    return samples[index];
}

GpuSurfaceDescriptor make_surface() {
    GpuSurfaceDescriptor surface;
    surface.surface_id = 0x4D4554414C42ULL;
    surface.generation_id = 1U;
    surface.width = 640U;
    surface.height = 360U;
    surface.format = GpuSurfaceFormat::Bgra8Unorm;
    return surface;
}

} // namespace

int main(int argc, char** argv) {
    using namespace zevryon::text;

    const std::uint32_t iterations = argc > 1
        ? static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10))
        : 256U;
    if (iterations == 0U) {
        return 2;
    }

    auto host = test::make_metal_window_test_host(640U, 360U);
    auto owner = make_metal_window_native_gpu_sdk_api();
    auto presenter = make_metal_native_window_swapchain_api();
    if (host == nullptr || owner == nullptr || presenter == nullptr) {
        return 3;
    }

    const NativeWindowSurfaceHandle window = host->handle();
    NativeGpuSdkConfig owner_config;
    owner_config.api_kind = NativeGpuApiKind::Metal;
    owner_config.require_real_device = 1U;
    owner_config.allow_software_device = 0U;
    owner_config.device_generation = 0x4D4554414C4201ULL;
    owner_config.runtime_generation = 0x4D4554414C4202ULL;
    owner_config.limits = default_native_gpu_sdk_limits(NativeGpuApiKind::Metal);
    owner_config.window = window;

    NativeGpuSdkError owner_error;
    if (!owner->initialize(owner_config, &owner_error)) {
        std::cerr << owner_error.message << '\n';
        return 4;
    }
    NativeGpuSdkContextHandle context;
    if (!owner->export_context(&context, &owner_error)) {
        std::cerr << owner_error.message << '\n';
        return 5;
    }

    NativeWindowSwapchainConfig config;
    config.context = context;
    config.window = window;
    config.surface = make_surface();
    config.limits = default_native_window_swapchain_limits(
        NativeGpuApiKind::Metal,
        NativeWindowSystem::CocoaLayer);
    config.limits.maximum_image_count = 3U;
    config.limits.maximum_frames_in_flight = 2U;
    config.limits.maximum_damage_rects = 64U;
    config.swapchain_generation = 1U;
    config.present_mode = NativePresentMode::Immediate;
    config.image_count = 3U;
    config.flags = kNativeWindowSwapchainRequireNativeContext |
                   kNativeWindowSwapchainAllowImmediate;

    NativeWindowSwapchainError error;
    if (!presenter->configure(config, &error)) {
        std::cerr << error.message << '\n';
        return 6;
    }
    owner->shutdown();

    std::vector<double> samples;
    samples.reserve(iterations);
    std::uint64_t checksum = kFnvOffset;
    std::uint32_t presented = 0U;
    std::uint32_t occluded = 0U;
    std::uint32_t out_of_date = 0U;

    std::uint32_t completed = 0U;
    std::uint32_t attempts = 0U;
    const std::uint32_t maximum_attempts = iterations * 16U;
    while (completed < iterations && attempts < maximum_attempts) {
        const std::uint32_t index = completed;
        ++attempts;
        const auto start = std::chrono::steady_clock::now();
        NativeWindowSwapchainImage image;
        NativeWindowAcquireStatus acquire_status{};
        if (!presenter->acquire(index + 1U, &image, &acquire_status, &error)) {
            std::cerr << error.message << '\n';
            return 7;
        }
        if (acquire_status == NativeWindowAcquireStatus::Occluded) {
            ++occluded;
            host->set_visible(true);
            host->pump_events();
            continue;
        }
        if (acquire_status == NativeWindowAcquireStatus::OutOfDate) {
            ++out_of_date;
            return 8;
        }
        if (acquire_status != NativeWindowAcquireStatus::Acquired) {
            host->pump_events();
            continue;
        }

        NativeWindowPresentRequest request;
        request.image = image;
        request.frame_id = static_cast<std::uint64_t>(index) + 1U;
        request.ticket_id = static_cast<std::uint64_t>(index) + 10'001U;
        request.command_checksum =
            0xD6E8FEB86659FD93ULL ^
            (static_cast<std::uint64_t>(index) * 0x9E3779B97F4A7C15ULL);
        request.command_count = 1U;
        request.flags = kNativeWindowPresentFullRedraw;
        NativeWindowPresentReceipt receipt;
        if (!presenter->present(request, &receipt, &error)) {
            std::cerr << error.message << '\n';
            return 9;
        }
        if (receipt.status != NativeWindowPresentStatus::Presented) {
            return 10;
        }
        if (!presenter->retire_completed(receipt.signal_fence_value, &error)) {
            std::cerr << error.message << '\n';
            return 11;
        }
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(
            end - start).count());
        ++presented;
        ++completed;
        hash_value(&checksum, receipt.frame_id);
        hash_value(&checksum, receipt.signal_fence_value);
        hash_value(&checksum, receipt.command_checksum);
        const std::uint8_t status = static_cast<std::uint8_t>(receipt.status);
        hash_value(&checksum, status);
    }
    if (completed != iterations) {
        return 12;
    }

    const NativeWindowSwapchainSnapshot snapshot = presenter->snapshot();
    const double p50 = percentile(samples, 0.50);
    const double p95 = percentile(samples, 0.95);
    const double p99 = percentile(samples, 0.99);
    const double maximum = *std::max_element(samples.begin(), samples.end());

    std::cout << "schema=zevryon.native-metal-window-benchmark.v1\n";
    std::cout << "iterations=" << iterations << '\n';
    std::cout << "samples=" << samples.size() << '\n';
    std::cout << "presented=" << presented << '\n';
    std::cout << "occluded=" << occluded << '\n';
    std::cout << "out_of_date=" << out_of_date << '\n';
    std::cout << "surface_bytes=" << snapshot.current_surface_bytes << '\n';
    std::cout << "peak_in_flight_bytes=" << snapshot.peak_in_flight_bytes << '\n';
    std::cout << "checksum=" << checksum << '\n';
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "p50_ms=" << p50 << '\n';
    std::cout << "p95_ms=" << p95 << '\n';
    std::cout << "p99_ms=" << p99 << '\n';
    std::cout << "max_ms=" << maximum << '\n';
    return 0;
}
