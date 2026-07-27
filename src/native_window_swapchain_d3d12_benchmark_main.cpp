#include "native_window_swapchain.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace zevryon::text;

LRESULT CALLBACK benchmark_window_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

class BenchmarkWindow final {
public:
    BenchmarkWindow() {
        instance_ = GetModuleHandleW(nullptr);
        const wchar_t* class_name = L"ZevryonZ2F8B2ABenchmarkWindow";
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = benchmark_window_proc;
        window_class.hInstance = instance_;
        window_class.lpszClassName = class_name;
        atom_ = RegisterClassExW(&window_class);
        window_ = CreateWindowExW(
            0U,
            class_name,
            L"Zevryon D3D12 WSI benchmark",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            640,
            360,
            nullptr,
            nullptr,
            instance_,
            nullptr);
        if (window_ != nullptr) {
            ShowWindow(window_, SW_SHOW);
            UpdateWindow(window_);
        }
    }

    ~BenchmarkWindow() {
        if (window_ != nullptr) {
            DestroyWindow(window_);
        }
        if (atom_ != 0U) {
            UnregisterClassW(L"ZevryonZ2F8B2ABenchmarkWindow", instance_);
        }
    }

    HWND get() const noexcept { return window_; }

private:
    HINSTANCE instance_{nullptr};
    ATOM atom_{0U};
    HWND window_{nullptr};
};

double percentile(std::vector<double> values, double quantile) {
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        quantile * static_cast<double>(values.size() - 1U));
    return values[index];
}

} // namespace

int main() {
    using namespace zevryon::text;
    constexpr std::uint32_t width = 640U;
    constexpr std::uint32_t height = 360U;
    constexpr std::uint64_t iterations = 256U;

    BenchmarkWindow window;
    assert(window.get() != nullptr);

    auto sdk = make_direct3d12_native_gpu_sdk_api();
    assert(sdk != nullptr);
    NativeGpuSdkConfig sdk_config;
    sdk_config.api_kind = NativeGpuApiKind::Direct3D12;
    sdk_config.allow_software_device = 1U;
    sdk_config.device_generation = 901U;
    sdk_config.runtime_generation = 902U;
    sdk_config.limits = default_native_gpu_sdk_limits(NativeGpuApiKind::Direct3D12);
    sdk_config.window.system = NativeWindowSystem::Headless;
    NativeGpuSdkError sdk_error;
    assert(sdk->initialize(sdk_config, &sdk_error));
    NativeGpuSdkContextHandle context;
    assert(sdk->export_context(&context, &sdk_error));

    auto api = make_direct3d12_native_window_swapchain_api();
    assert(api != nullptr);
    NativeWindowSwapchainConfig config;
    config.context = context;
    config.window.generation = 903U;
    config.window.window_or_layer = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(window.get()));
    config.window.system = NativeWindowSystem::Win32;
    config.surface.surface_id = 904U;
    config.surface.generation_id = 1U;
    config.surface.width = width;
    config.surface.height = height;
    config.surface.format = GpuSurfaceFormat::Bgra8Unorm;
    config.limits = default_native_window_swapchain_limits(
        NativeGpuApiKind::Direct3D12,
        NativeWindowSystem::Win32);
    config.swapchain_generation = 1U;
    config.present_mode = NativePresentMode::Immediate;
    config.image_count = 3U;
    config.flags = kNativeWindowSwapchainAllowImmediate |
                   kNativeWindowSwapchainAllowPartialPresent |
                   kNativeWindowSwapchainRequireNativeContext;
    NativeWindowSwapchainError error;
    assert(api->configure(config, &error));

    const std::array<NativeDamageRect, 1U> damage{{
        NativeDamageRect{0, 0, width, height}}};
    std::vector<double> milliseconds;
    milliseconds.reserve(iterations);
    std::uint64_t aggregate_checksum = 1469598103934665603ULL;
    std::uint64_t presented = 0U;
    std::uint64_t occluded = 0U;

    for (std::uint64_t iteration = 0U; iteration < iterations; ++iteration) {
        const auto begin = std::chrono::steady_clock::now();
        NativeWindowSwapchainImage image;
        NativeWindowAcquireStatus acquire_status{};
        assert(api->acquire(iteration + 1U, &image, &acquire_status, &error));
        if (acquire_status == NativeWindowAcquireStatus::Occluded) {
            ++occluded;
            continue;
        }
        assert(acquire_status == NativeWindowAcquireStatus::Acquired);
        NativeWindowPresentRequest request;
        request.image = image;
        request.damage_rects = damage;
        request.frame_id = iteration + 1U;
        request.ticket_id = iteration + 10'000U;
        request.command_checksum = 0xD312000000000000ULL ^ iteration;
        request.command_count = 80U;
        request.flags = kNativeWindowPresentFullRedraw;
        NativeWindowPresentReceipt receipt;
        assert(api->present(request, &receipt, &error));
        assert(receipt.status == NativeWindowPresentStatus::Presented ||
               receipt.status == NativeWindowPresentStatus::Occluded);
        assert(api->retire_completed(receipt.signal_fence_value, &error));
        if (receipt.status == NativeWindowPresentStatus::Presented) {
            ++presented;
        } else {
            ++occluded;
        }
        aggregate_checksum ^= receipt.command_checksum;
        aggregate_checksum *= 1099511628211ULL;
        aggregate_checksum ^= receipt.signal_fence_value;
        aggregate_checksum *= 1099511628211ULL;
        const auto end = std::chrono::steady_clock::now();
        milliseconds.push_back(
            std::chrono::duration<double, std::milli>(end - begin).count());
    }

    assert(!milliseconds.empty());
    const NativeWindowSwapchainSnapshot snapshot = api->snapshot();
    std::cout << "iterations=" << iterations << '\n';
    std::cout << "samples=" << milliseconds.size() << '\n';
    std::cout << "presented=" << presented << '\n';
    std::cout << "occluded=" << occluded << '\n';
    std::cout << "p50_ms=" << percentile(milliseconds, 0.50) << '\n';
    std::cout << "p95_ms=" << percentile(milliseconds, 0.95) << '\n';
    std::cout << "p99_ms=" << percentile(milliseconds, 0.99) << '\n';
    std::cout << "max_ms="
              << *std::max_element(milliseconds.begin(), milliseconds.end())
              << '\n';
    std::cout << "surface_bytes=" << snapshot.current_surface_bytes << '\n';
    std::cout << "peak_in_flight_bytes=" << snapshot.peak_in_flight_bytes << '\n';
    std::cout << "checksum=" << aggregate_checksum << '\n';
    std::cout << "software_device="
              << (((context.flags & kNativeGpuSdkContextSoftwareDevice) != 0U) ? 1 : 0)
              << '\n';

    api->shutdown();
    sdk->shutdown();
    return 0;
}
