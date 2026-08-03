#include "native_shader_execution.hpp"
#include "native_window_swapchain.hpp"
#include "shader_draw_packet_fixture.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <vector>

namespace {

using namespace zevryon::text;
using namespace zevryon::text::test;

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
        class_name_ = L"ZevryonZ2F8B3B3AIntegrationWindow";
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
            L"Zevryon Z2F-8B3B3A integration",
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

struct PacketState final {
    std::vector<std::byte> storage;
    std::pmr::monotonic_buffer_resource resource;
    GpuShaderPacket packet;
    ShaderAtlasResidency atlas;
    ShaderReadback reference;

    PacketState()
        : storage(2U * 1024U * 1024U),
          resource(storage.data(), storage.size()),
          packet(&resource),
          atlas(8U, 1U << 20U) {}
};

bool prepare_packet(PacketState* output) {
    if (output == nullptr) {
        return false;
    }
    ShaderPacketFixture fixture = make_shader_packet_fixture();
    ShaderPacketError error;
    return compile_gpu_shader_packet(fixture.input(), &output->packet, &error) &&
        output->atlas.apply_packet_uploads(output->packet, &error) &&
        execute_shader_packet_reference(
            output->packet, output->atlas, &output->reference, &error);
}

NativeGpuSdkConfig make_sdk_config() {
    NativeGpuSdkConfig config{};
    config.api_kind = NativeGpuApiKind::Direct3D12;
    config.allow_software_device = 1U;
    config.require_real_device = 0U;
    config.enable_validation = 0U;
    config.device_generation = 301U;
    config.runtime_generation = 307U;
    config.limits = default_native_gpu_sdk_limits(NativeGpuApiKind::Direct3D12);
    config.window.system = NativeWindowSystem::Headless;
    config.window.generation = 311U;
    return config;
}

NativeWindowSwapchainConfig make_window_config(
    HWND window,
    const NativeGpuSdkContextHandle& context,
    std::uint32_t width,
    std::uint32_t height) {
    NativeWindowSwapchainConfig config{};
    config.context = context;
    config.window.generation = 313U;
    config.window.window_or_layer = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(window));
    config.window.system = NativeWindowSystem::Win32;
    config.surface.surface_id = 317U;
    config.surface.generation_id = 1U;
    config.surface.width = width;
    config.surface.height = height;
    config.surface.format = GpuSurfaceFormat::Bgra8Unorm;
    config.limits = default_native_window_swapchain_limits(
        NativeGpuApiKind::Direct3D12,
        NativeWindowSystem::Win32);
    config.limits.maximum_frames_in_flight = 2U;
    config.swapchain_generation = 1U;
    config.present_mode = NativePresentMode::Immediate;
    config.image_count = 3U;
    config.flags = kNativeWindowSwapchainAllowImmediate |
                   kNativeWindowSwapchainAllowPartialPresent |
                   kNativeWindowSwapchainRequireNativeContext;
    return config;
}

NativeWindowPresentRequest make_surface_request(
    const NativeWindowSwapchainImage& image,
    const NativeShaderSurfaceView& surface,
    std::span<const NativeDamageRect> damage,
    std::uint64_t frame_id) {
    NativeWindowPresentRequest request{};
    request.image = image;
    request.damage_rects = damage;
    request.frame_id = frame_id;
    request.ticket_id = frame_id + 1000U;
    request.command_checksum = surface.content_checksum;
    request.command_count = 1U;
    request.flags = kNativeWindowPresentFullRedraw;
    request.shader_surface = surface;
    return request;
}

bool presented_or_occluded(NativeWindowPresentStatus status) noexcept {
    return status == NativeWindowPresentStatus::Presented ||
        status == NativeWindowPresentStatus::Occluded;
}

} // namespace

int main() {
    assert(native_shader_execution_build_has_backend(
        NativeGpuApiKind::Direct3D12));
    assert(native_window_swapchain_build_has_backend(
        NativeGpuApiKind::Direct3D12,
        NativeWindowSystem::Win32));

    PacketState packet;
    assert(prepare_packet(&packet));
    assert(packet.packet.header.surface_width != 0U);
    assert(packet.packet.header.surface_height != 0U);

    std::unique_ptr<NativeGpuSdkApi> owner =
        make_direct3d12_native_gpu_sdk_api();
    assert(owner != nullptr);
    NativeGpuSdkError sdk_error;
    assert(owner->initialize(make_sdk_config(), &sdk_error));
    NativeGpuSdkContextHandle context;
    assert(owner->export_context(&context, &sdk_error));

    std::unique_ptr<NativeShaderExecutor> executor =
        make_direct3d12_native_shader_executor();
    assert(executor != nullptr);
    NativeShaderExecutionConfig execution_config{};
    execution_config.context = context;
    execution_config.limits = default_native_shader_execution_limits(
        NativeGpuApiKind::Direct3D12);
    execution_config.executor_generation = 331U;
    NativeShaderExecutionError execution_error;
    assert(executor->configure(execution_config, &execution_error));

    TestWindow window(
        packet.packet.header.surface_width,
        packet.packet.header.surface_height);
    assert(window.get() != nullptr);
    std::unique_ptr<NativeWindowSwapchainApi> swapchain =
        make_direct3d12_native_window_swapchain_api();
    assert(swapchain != nullptr);
    NativeWindowSwapchainError window_error;
    assert(swapchain->configure(
        make_window_config(
            window.get(),
            context,
            packet.packet.header.surface_width,
            packet.packet.header.surface_height),
        &window_error));

    owner->shutdown();
    owner.reset();

    const std::array<NativeDamageRect, 1U> full_damage{{NativeDamageRect{
        0,
        0,
        packet.packet.header.surface_width,
        packet.packet.header.surface_height}}};

    NativeWindowAcquireStatus acquire_status{};
    NativeWindowSwapchainImage first_image;
    assert(swapchain->acquire(
        1U, &first_image, &acquire_status, &window_error));
    assert(acquire_status == NativeWindowAcquireStatus::Acquired);

    assert(executor->execute(
        packet.packet, packet.atlas, nullptr, &execution_error));
    NativeShaderSurfaceView first_surface;
    assert(executor->export_surface(&first_surface, &execution_error));
    assert(first_surface.native_resource != 0U);
    assert(first_surface.content_checksum == packet.packet.header.packet_checksum);

    std::vector<std::byte> mixed_storage(
        static_cast<std::size_t>(first_surface.width) *
        first_surface.height * 4U);
    NativeWindowPresentRequest mixed_request = make_surface_request(
        first_image, first_surface, full_damage, 1U);
    mixed_request.pixel_buffer.bytes = mixed_storage;
    mixed_request.pixel_buffer.width = first_surface.width;
    mixed_request.pixel_buffer.height = first_surface.height;
    mixed_request.pixel_buffer.row_bytes = first_surface.width * 4U;
    mixed_request.pixel_buffer.format = first_surface.format;
    mixed_request.pixel_buffer.premultiplied_alpha = 1U;
    NativeWindowPresentReceipt rejected_receipt;
    assert(!swapchain->present(
        mixed_request, &rejected_receipt, &window_error));
    assert(window_error.kind == NativeWindowSwapchainErrorKind::InvalidInput);

    NativeWindowPresentRequest first_request = make_surface_request(
        first_image, first_surface, full_damage, 2U);
    NativeWindowPresentReceipt first_receipt;
    assert(swapchain->present(
        first_request, &first_receipt, &window_error));
    assert(presented_or_occluded(first_receipt.status));
    assert(first_receipt.signal_fence_value != 0U);

    NativeWindowSwapchainImage second_image;
    assert(swapchain->acquire(
        2U, &second_image, &acquire_status, &window_error));
    assert(acquire_status == NativeWindowAcquireStatus::Acquired);

    assert(executor->execute(
        packet.packet, packet.atlas, nullptr, &execution_error));
    NativeShaderSurfaceView second_surface;
    assert(executor->export_surface(&second_surface, &execution_error));

    NativeShaderSurfaceView stale_surface = second_surface;
    stale_surface.runtime_generation += 1U;
    NativeWindowPresentRequest stale_request = make_surface_request(
        second_image, stale_surface, full_damage, 3U);
    assert(!swapchain->present(
        stale_request, &rejected_receipt, &window_error));
    assert(window_error.kind == NativeWindowSwapchainErrorKind::StaleGeneration);

    NativeWindowPresentRequest second_request = make_surface_request(
        second_image, second_surface, full_damage, 4U);
    NativeWindowPresentReceipt second_receipt;
    assert(swapchain->present(
        second_request, &second_receipt, &window_error));
    assert(presented_or_occluded(second_receipt.status));
    assert(second_receipt.signal_fence_value > first_receipt.signal_fence_value);

    NativeWindowSwapchainImage blocked_image;
    assert(swapchain->acquire(
        3U, &blocked_image, &acquire_status, &window_error));
    assert(acquire_status == NativeWindowAcquireStatus::NotReady);

    assert(swapchain->retire_completed(
        second_receipt.signal_fence_value, &window_error));
    const NativeWindowSwapchainSnapshot window_snapshot = swapchain->snapshot();
    assert(window_snapshot.presented_frames == 2U ||
           window_snapshot.occlusion_events != 0U);
    assert(window_snapshot.in_flight_frame_count == 0U);

    const NativeShaderExecutionSnapshot execution_snapshot =
        executor->snapshot();
    assert(execution_snapshot.executions == 2U);
    assert(execution_snapshot.readbacks == 0U);
    assert((execution_snapshot.capability_flags &
        kNativeShaderExecutionDirectSurfaceExport) != 0U);

    swapchain->shutdown();
    executor->shutdown();
    std::cout << "direct D3D12 shader-surface presentation: frames=2 PASS\n";
    return 0;
}
