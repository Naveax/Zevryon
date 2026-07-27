#include "native_window_swapchain.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>

namespace {

using namespace zevryon::text;

LRESULT CALLBACK test_window_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

class TestWindow final {
public:
    TestWindow(std::uint32_t width, std::uint32_t height) {
        instance_ = GetModuleHandleW(nullptr);
        class_name_ = L"ZevryonZ2F8B2ATestWindow";
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = test_window_proc;
        window_class.hInstance = instance_;
        window_class.lpszClassName = class_name_;
        atom_ = RegisterClassExW(&window_class);
        if (atom_ == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return;
        }
        window_ = CreateWindowExW(
            0U,
            class_name_,
            L"Zevryon Z2F-8B2A",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            static_cast<int>(width),
            static_cast<int>(height),
            nullptr,
            nullptr,
            instance_,
            nullptr);
        if (window_ != nullptr) {
            ShowWindow(window_, SW_SHOW);
            UpdateWindow(window_);
            pump();
        }
    }

    ~TestWindow() {
        if (window_ != nullptr) {
            DestroyWindow(window_);
        }
        if (atom_ != 0U) {
            UnregisterClassW(class_name_, instance_);
        }
    }

    HWND get() const noexcept { return window_; }

    static void pump() noexcept {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

private:
    HINSTANCE instance_{nullptr};
    const wchar_t* class_name_{nullptr};
    ATOM atom_{0U};
    HWND window_{nullptr};
};

NativeGpuSdkConfig make_sdk_config() {
    NativeGpuSdkConfig config;
    config.api_kind = NativeGpuApiKind::Direct3D12;
    config.allow_software_device = 1U;
    config.require_real_device = 0U;
    config.device_generation = 71U;
    config.runtime_generation = 81U;
    config.limits = default_native_gpu_sdk_limits(NativeGpuApiKind::Direct3D12);
    config.window.system = NativeWindowSystem::Headless;
    return config;
}

NativeWindowSwapchainConfig make_window_config(
    HWND window,
    const NativeGpuSdkContextHandle& context,
    std::uint32_t width,
    std::uint32_t height,
    std::uint64_t surface_generation,
    std::uint64_t swapchain_generation) {
    NativeWindowSwapchainConfig config;
    config.context = context;
    config.window.generation = 101U;
    config.window.window_or_layer = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(window));
    config.window.system = NativeWindowSystem::Win32;
    config.surface.surface_id = 201U;
    config.surface.generation_id = surface_generation;
    config.surface.width = width;
    config.surface.height = height;
    config.surface.format = GpuSurfaceFormat::Bgra8Unorm;
    config.limits = default_native_window_swapchain_limits(
        NativeGpuApiKind::Direct3D12,
        NativeWindowSystem::Win32);
    config.swapchain_generation = swapchain_generation;
    config.present_mode = NativePresentMode::Immediate;
    config.image_count = 3U;
    config.flags = kNativeWindowSwapchainAllowImmediate |
                   kNativeWindowSwapchainAllowPartialPresent |
                   kNativeWindowSwapchainRequireNativeContext;
    return config;
}

NativeWindowPresentReceipt present_full(
    NativeWindowSwapchainApi* api,
    const NativeWindowSwapchainImage& image,
    std::uint64_t frame_id,
    std::uint32_t width,
    std::uint32_t height) {
    const std::array<NativeDamageRect, 1U> damage{{
        NativeDamageRect{0, 0, width, height}}};
    NativeWindowPresentRequest request;
    request.image = image;
    request.damage_rects = damage;
    request.frame_id = frame_id;
    request.ticket_id = frame_id + 1000U;
    request.command_checksum = 0xA5000000ULL + frame_id;
    request.command_count = 1U;
    request.flags = kNativeWindowPresentFullRedraw;
    NativeWindowPresentReceipt receipt;
    NativeWindowSwapchainError error;
    assert(api->present(request, &receipt, &error));
    assert(receipt.status == NativeWindowPresentStatus::Presented ||
           receipt.status == NativeWindowPresentStatus::Occluded);
    assert(receipt.signal_fence_value != 0U);
    return receipt;
}

} // namespace

int main() {
    using namespace zevryon::text;

    assert(native_window_swapchain_build_has_backend(
        NativeGpuApiKind::Direct3D12,
        NativeWindowSystem::Win32));

    auto sdk = make_direct3d12_native_gpu_sdk_api();
    assert(sdk != nullptr);
    NativeGpuSdkError sdk_error;
    NativeGpuSdkContextHandle context;
    assert(!sdk->export_context(&context, &sdk_error));
    assert(sdk_error.kind == NativeGpuSdkErrorKind::RuntimeUnavailable);
    assert(sdk->initialize(make_sdk_config(), &sdk_error));
    assert(sdk->export_context(&context, &sdk_error));
    assert(context.api_kind == NativeGpuApiKind::Direct3D12);
    assert(context.device_generation == 71U);
    assert(context.runtime_generation == 81U);
    assert(context.instance_or_factory != 0U);
    assert(context.physical_device_or_adapter != 0U);
    assert(context.device != 0U);
    assert(context.graphics_queue != 0U);
    assert(context.present_queue == context.graphics_queue);
    assert((context.flags & kNativeGpuSdkContextSharedGraphicsPresentQueue) != 0U);

    TestWindow window(640U, 480U);
    assert(window.get() != nullptr);
    auto api = make_direct3d12_native_window_swapchain_api();
    assert(api != nullptr);
    NativeWindowSwapchainError error;
    NativeWindowSwapchainConfig config = make_window_config(
        window.get(), context, 640U, 480U, 1U, 1U);
    assert(api->configure(config, &error));

    // The presenter must retain the exported COM graph independently. Closing
    // the Z2F-8A owner after configuration must not create another device or
    // invalidate the real swapchain, queue, back buffers, or resize path.
    sdk->shutdown();

    NativeWindowSwapchainImage first_image;
    NativeWindowAcquireStatus acquire_status{};
    assert(api->acquire(1U, &first_image, &acquire_status, &error));
    assert(acquire_status == NativeWindowAcquireStatus::Acquired);
    const NativeWindowPresentReceipt first_receipt = present_full(
        api.get(), first_image, 1U, 640U, 480U);
    assert(api->retire_completed(first_receipt.signal_fence_value, &error));

    NativeWindowSwapchainImage skipped_image;
    assert(api->acquire(2U, &skipped_image, &acquire_status, &error));
    assert(acquire_status == NativeWindowAcquireStatus::Acquired);
    NativeWindowPresentRequest skip_request;
    skip_request.image = skipped_image;
    skip_request.frame_id = 2U;
    skip_request.ticket_id = 1002U;
    NativeWindowPresentReceipt skip_receipt;
    assert(api->present(skip_request, &skip_receipt, &error));
    assert(skip_receipt.status == NativeWindowPresentStatus::SkippedNoDamage);

    NativeWindowSwapchainImage image_a;
    NativeWindowSwapchainImage image_b;
    assert(api->acquire(3U, &image_a, &acquire_status, &error));
    assert(acquire_status == NativeWindowAcquireStatus::Acquired);
    const NativeWindowPresentReceipt receipt_a = present_full(
        api.get(), image_a, 3U, 640U, 480U);
    assert(api->acquire(4U, &image_b, &acquire_status, &error));
    assert(acquire_status == NativeWindowAcquireStatus::Acquired);
    const NativeWindowPresentReceipt receipt_b = present_full(
        api.get(), image_b, 4U, 640U, 480U);
    NativeWindowSwapchainImage blocked;
    assert(api->acquire(5U, &blocked, &acquire_status, &error));
    assert(acquire_status == NativeWindowAcquireStatus::NotReady);
    assert(api->retire_completed(receipt_b.signal_fence_value, &error));
    assert(receipt_b.signal_fence_value > receipt_a.signal_fence_value);

    GpuSurfaceDescriptor resized = config.surface;
    resized.generation_id = 2U;
    resized.width = 800U;
    resized.height = 600U;
    assert(api->request_resize(resized, &error));
    assert(api->acquire(6U, &blocked, &acquire_status, &error));
    assert(acquire_status == NativeWindowAcquireStatus::OutOfDate);

    NativeWindowSwapchainConfig recreated = config;
    recreated.surface = resized;
    recreated.swapchain_generation = 2U;
    assert(api->recreate(recreated, &error));
    NativeWindowSwapchainImage resized_image;
    assert(api->acquire(7U, &resized_image, &acquire_status, &error));
    assert(acquire_status == NativeWindowAcquireStatus::Acquired);

    const std::array<NativeDamageRect, 1U> stale_damage{{
        NativeDamageRect{0, 0, 640U, 480U}}};
    NativeWindowPresentRequest stale_request;
    stale_request.image = first_image;
    stale_request.damage_rects = stale_damage;
    stale_request.frame_id = 8U;
    stale_request.ticket_id = 1008U;
    stale_request.flags = kNativeWindowPresentFullRedraw;
    NativeWindowPresentReceipt stale_receipt;
    assert(!api->present(stale_request, &stale_receipt, &error));
    assert(error.kind == NativeWindowSwapchainErrorKind::StaleGeneration);

    const NativeWindowPresentReceipt resized_receipt = present_full(
        api.get(), resized_image, 9U, 800U, 600U);
    assert(api->retire_completed(resized_receipt.signal_fence_value, &error));

    const NativeWindowSwapchainSnapshot snapshot = api->snapshot();
    assert(snapshot.configurations == 1U);
    assert(snapshot.recreations == 1U);
    assert(snapshot.resize_requests == 1U);
    assert(snapshot.configured_image_count == 3U);
    assert(snapshot.acquired_image_count == 0U);
    assert(snapshot.in_flight_frame_count == 0U);
    assert(snapshot.stale_rejections >= 1U);
    assert(snapshot.current_surface_bytes == 800ULL * 600ULL * 4ULL * 3ULL);

    api->shutdown();
    TestWindow::pump();
    return 0;
}
