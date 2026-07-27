#include "native_window_swapchain.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <span>
#include <vector>

namespace {
using namespace zevryon::text;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

template <typename T>
void hash_value(std::uint64_t* hash, const T& value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        *hash ^= bytes[index];
        *hash *= kFnvPrime;
    }
}

NativeWindowSystem system_for(NativeGpuApiKind kind) {
    switch (kind) {
        case NativeGpuApiKind::Vulkan: return NativeWindowSystem::Xcb;
        case NativeGpuApiKind::Metal: return NativeWindowSystem::CocoaLayer;
        case NativeGpuApiKind::Direct3D12: return NativeWindowSystem::Win32;
        case NativeGpuApiKind::ReferenceCpu: break;
    }
    return NativeWindowSystem::Headless;
}

NativeWindowSwapchainConfig config_for(NativeGpuApiKind kind) {
    const NativeWindowSystem system = system_for(kind);
    NativeWindowSwapchainConfig config;
    config.context.api_kind = kind;
    config.context.flags =
        kNativeGpuSdkContextDeviceValid |
        kNativeGpuSdkContextGraphicsQueueValid |
        kNativeGpuSdkContextPresentQueueValid |
        kNativeGpuSdkContextSharedGraphicsPresentQueue;
    config.context.device_generation = 17U;
    config.context.runtime_generation = 23U;
    config.context.instance_or_factory = 29U;
    config.context.physical_device_or_adapter = 31U;
    config.context.device = 37U;
    config.context.graphics_queue = 41U;
    config.context.present_queue = 41U;
    config.window.generation = 43U;
    config.window.display_or_instance = 47U;
    config.window.window_or_layer = 53U;
    config.window.system = system;
    config.surface.surface_id = 59U;
    config.surface.generation_id = 61U;
    config.surface.width = 1920U;
    config.surface.height = 1080U;
    config.surface.format = GpuSurfaceFormat::Bgra8Unorm;
    config.limits = default_native_window_swapchain_limits(kind, system);
    config.swapchain_generation = 67U;
    config.present_mode = NativePresentMode::Fifo;
    config.image_count = 3U;
    config.flags =
        kNativeWindowSwapchainAllowPartialPresent |
        kNativeWindowSwapchainRequireNativeContext;
    return config;
}

struct Backend final {
    NativeGpuApiKind kind{NativeGpuApiKind::ReferenceCpu};
    ReferenceNativeWindowSwapchainApi api;
    std::uint64_t checksum{kFnvOffset};

    Backend(
        NativeGpuApiKind kind_value,
        NativeWindowSwapchainCapabilities capabilities)
        : kind(kind_value), api(capabilities) {}
};

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        fraction * static_cast<double>(values.size() - 1U));
    return values[index];
}

void execute_iteration(
    Backend* backend,
    std::uint64_t iteration,
    std::span<const NativeDamageRect> damage) {
    NativeWindowSwapchainError error;
    NativeWindowSwapchainImage image;
    NativeWindowAcquireStatus acquire = NativeWindowAcquireStatus::NotReady;
    if (!backend->api.acquire(iteration + 1U, &image, &acquire, &error) ||
        acquire != NativeWindowAcquireStatus::Acquired) {
        std::cerr << "acquire failed: " << error.message << "\n";
        std::abort();
    }
    NativeWindowPresentRequest request;
    request.image = image;
    request.damage_rects = damage;
    request.frame_id = 1000U + iteration;
    request.ticket_id = 2000U + iteration;
    request.wait_fence_value = iteration;
    request.command_checksum = 0xA5A5000000000000ULL | iteration;
    request.command_count = 80U;
    NativeWindowPresentReceipt receipt;
    if (!backend->api.present(request, &receipt, &error) ||
        receipt.status != NativeWindowPresentStatus::Presented) {
        std::cerr << "present failed: " << error.message << "\n";
        std::abort();
    }
    if (!backend->api.retire_completed(receipt.signal_fence_value, &error)) {
        std::cerr << "retire failed: " << error.message << "\n";
        std::abort();
    }
    hash_value(&backend->checksum, backend->kind);
    hash_value(&backend->checksum, receipt.image.swapchain_generation);
    hash_value(&backend->checksum, receipt.image.image.image.image_generation);
    hash_value(&backend->checksum, receipt.image.acquire_serial);
    hash_value(&backend->checksum, receipt.image.present_serial);
    hash_value(&backend->checksum, receipt.frame_id);
    hash_value(&backend->checksum, receipt.ticket_id);
    hash_value(&backend->checksum, receipt.signal_fence_value);
    hash_value(&backend->checksum, receipt.command_checksum);
    hash_value(&backend->checksum, receipt.command_count);
    hash_value(&backend->checksum, receipt.damage_rect_count);
    hash_value(&backend->checksum, receipt.status);
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t iterations = 512U;
    if (argc > 1) {
        const unsigned long parsed = std::strtoul(argv[1], nullptr, 10);
        if (parsed == 0UL || parsed > 100000UL) {
            return 2;
        }
        iterations = static_cast<std::uint32_t>(parsed);
    }

    std::array<Backend, 3U> backends{{
        {
            NativeGpuApiKind::Vulkan,
            default_native_window_swapchain_capabilities(
                NativeGpuApiKind::Vulkan, NativeWindowSystem::Xcb),
        },
        {
            NativeGpuApiKind::Metal,
            default_native_window_swapchain_capabilities(
                NativeGpuApiKind::Metal, NativeWindowSystem::CocoaLayer),
        },
        {
            NativeGpuApiKind::Direct3D12,
            default_native_window_swapchain_capabilities(
                NativeGpuApiKind::Direct3D12, NativeWindowSystem::Win32),
        },
    }};
    for (Backend& backend : backends) {
        NativeWindowSwapchainError error;
        if (!backend.api.configure(config_for(backend.kind), &error)) {
            std::cerr << "configure failed: " << error.message << "\n";
            return 3;
        }
    }

    const std::array<NativeDamageRect, 4U> damage{{
        {0, 0, 640U, 360U},
        {640, 0, 640U, 360U},
        {0, 360, 640U, 360U},
        {1280, 720, 640U, 360U},
    }};
    constexpr std::uint32_t warmup_iterations = 64U;
    for (std::uint32_t iteration = 0U; iteration < warmup_iterations; ++iteration) {
        for (Backend& backend : backends) {
            execute_iteration(&backend, iteration, damage);
        }
    }

    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::uint32_t iteration = 0U; iteration < iterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        for (Backend& backend : backends) {
            execute_iteration(
                &backend,
                static_cast<std::uint64_t>(warmup_iterations) + iteration,
                damage);
        }
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(end - start).count());
    }

    std::uint64_t checksum = kFnvOffset;
    std::uint64_t surface_bytes_total = 0U;
    std::uint64_t peak_in_flight_bytes_total = 0U;
    for (const Backend& backend : backends) {
        hash_value(&checksum, backend.checksum);
        const NativeWindowSwapchainSnapshot snapshot = backend.api.snapshot();
        surface_bytes_total += snapshot.current_surface_bytes;
        peak_in_flight_bytes_total += snapshot.peak_in_flight_bytes;
    }

    const double p50 = percentile(samples, 0.50);
    const double p95 = percentile(samples, 0.95);
    const double p99 = percentile(samples, 0.99);
    const double maximum = *std::max_element(samples.begin(), samples.end());

    std::cout << std::fixed << std::setprecision(6)
              << "{\n"
              << "  \"schema\": \"zevryon.native-window-swapchain-benchmark.v1\",\n"
              << "  \"input_document_lines\": 16384,\n"
              << "  \"input_projected_lines\": 80,\n"
              << "  \"backend_count\": 3,\n"
              << "  \"iterations\": " << iterations << ",\n"
              << "  \"submissions_per_iteration\": 3,\n"
              << "  \"damage_rects_per_submission\": 4,\n"
              << "  \"commands_per_submission\": 80,\n"
              << "  \"swapchain_images_per_backend\": 3,\n"
              << "  \"frames_in_flight_limit\": 2,\n"
              << "  \"surface_width\": 1920,\n"
              << "  \"surface_height\": 1080,\n"
              << "  \"surface_bytes_per_backend\": 24883200,\n"
              << "  \"surface_bytes_total\": " << surface_bytes_total << ",\n"
              << "  \"peak_in_flight_bytes_total\": "
              << peak_in_flight_bytes_total << ",\n"
              << "  \"context_record_bytes\": "
              << sizeof(NativeGpuSdkContextHandle) << ",\n"
              << "  \"capabilities_record_bytes\": "
              << sizeof(NativeWindowSwapchainCapabilities) << ",\n"
              << "  \"limits_record_bytes\": "
              << sizeof(NativeWindowSwapchainLimits) << ",\n"
              << "  \"config_record_bytes\": "
              << sizeof(NativeWindowSwapchainConfig) << ",\n"
              << "  \"image_record_bytes\": "
              << sizeof(NativeWindowSwapchainImage) << ",\n"
              << "  \"receipt_record_bytes\": "
              << sizeof(NativeWindowPresentReceipt) << ",\n"
              << "  \"vulkan_checksum\": " << backends[0].checksum << ",\n"
              << "  \"metal_checksum\": " << backends[1].checksum << ",\n"
              << "  \"d3d12_checksum\": " << backends[2].checksum << ",\n"
              << "  \"checksum\": " << checksum << ",\n"
              << "  \"p50_ms\": " << p50 << ",\n"
              << "  \"p95_ms\": " << p95 << ",\n"
              << "  \"p99_ms\": " << p99 << ",\n"
              << "  \"maximum_ms\": " << maximum << "\n"
              << "}\n";
    return 0;
}
