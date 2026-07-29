#include "native_shader_execution.hpp"
#include "shader_draw_packet_fixture.hpp"

#if defined(ZEVRYON_NATIVE_SHADER_TEST_METAL)
#include "native_metal_window.hpp"
#include "native_metal_window_test_window.hpp"
#elif defined(ZEVRYON_NATIVE_SHADER_TEST_VULKAN)
#include "native_vulkan_wsi.hpp"
#include "native_vulkan_wsi_test_window.hpp"
#endif

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

namespace {
using namespace zevryon::text;
using namespace zevryon::text::test;

NativeGpuApiKind backend_kind() noexcept {
#if defined(ZEVRYON_NATIVE_SHADER_TEST_D3D12)
    return NativeGpuApiKind::Direct3D12;
#elif defined(ZEVRYON_NATIVE_SHADER_TEST_VULKAN)
    return NativeGpuApiKind::Vulkan;
#elif defined(ZEVRYON_NATIVE_SHADER_TEST_METAL)
    return NativeGpuApiKind::Metal;
#else
    return NativeGpuApiKind::ReferenceCpu;
#endif
}

std::unique_ptr<NativeGpuSdkApi> make_owner() noexcept {
#if defined(ZEVRYON_NATIVE_SHADER_TEST_D3D12)
    return make_direct3d12_native_gpu_sdk_api();
#elif defined(ZEVRYON_NATIVE_SHADER_TEST_VULKAN)
    return make_vulkan_wsi_native_gpu_sdk_api();
#elif defined(ZEVRYON_NATIVE_SHADER_TEST_METAL)
    return make_metal_window_native_gpu_sdk_api();
#else
    return nullptr;
#endif
}

std::unique_ptr<NativeShaderExecutionApi> make_executor() noexcept {
#if defined(ZEVRYON_NATIVE_SHADER_TEST_D3D12)
    return make_direct3d12_native_shader_execution_api();
#elif defined(ZEVRYON_NATIVE_SHADER_TEST_VULKAN)
    return make_vulkan_native_shader_execution_api();
#elif defined(ZEVRYON_NATIVE_SHADER_TEST_METAL)
    return make_metal_native_shader_execution_api();
#else
    return nullptr;
#endif
}

} // namespace

int main() {
    const NativeGpuApiKind kind = backend_kind();
    std::unique_ptr<NativeGpuSdkApi> owner = make_owner();
    std::unique_ptr<NativeShaderExecutionApi> executor = make_executor();
    assert(owner != nullptr);
    assert(executor != nullptr);

    NativeGpuSdkConfig config;
    config.api_kind = kind;
    config.device_generation = 31U;
    config.runtime_generation = 41U;
    config.allow_software_device = 1U;
    config.require_real_device = 1U;
    config.limits = default_native_gpu_sdk_limits(kind);

#if defined(ZEVRYON_NATIVE_SHADER_TEST_VULKAN)
    NativeVulkanTestWindow window;
    std::string window_error;
#if defined(ZEVRYON_VULKAN_WSI_TEST_WAYLAND)
    const NativeWindowSystem system = NativeWindowSystem::Wayland;
#else
    const NativeWindowSystem system = NativeWindowSystem::Xcb;
#endif
    assert(window.create(system, 640U, 360U, &window_error));
    config.window = window.handle();
#elif defined(ZEVRYON_NATIVE_SHADER_TEST_METAL)
    std::unique_ptr<MetalWindowTestHost> window =
        make_metal_window_test_host(640U, 360U);
    assert(window != nullptr);
    config.window = window->handle();
#else
    config.window.system = NativeWindowSystem::Headless;
#endif

    NativeGpuSdkError owner_error;
    assert(owner->initialize(config, &owner_error));
    NativeGpuSdkContextHandle context;
    assert(owner->export_context(&context, &owner_error));

    NativeShaderExecutionError shader_error;
    const NativeShaderExecutionLimits limits =
        default_native_shader_execution_limits();
    assert(executor->configure(context, limits, &shader_error));

    // The executor must retain the exact device/queue graph independently of
    // the owner object, without creating a parallel GPU device.
    owner->shutdown();
    owner.reset();

    ShaderPacketFixture fixture = make_shader_packet_fixture();
    std::vector<std::byte> storage(2U * 1024U * 1024U);
    std::pmr::monotonic_buffer_resource arena(
        storage.data(), storage.size(), std::pmr::null_memory_resource());
    GpuShaderPacket packet(&arena);
    ShaderPacketError packet_error;
    assert(compile_gpu_shader_packet(fixture.input(), &packet, &packet_error));
    ShaderAtlasResidency atlas(8U, 1U << 20U);
    assert(atlas.apply_packet_uploads(packet, &packet_error));
    ShaderReadback oracle;
    assert(execute_shader_packet_reference(packet, atlas, &oracle, &packet_error));

    NativeShaderExecutionRequest request;
    request.packet = &packet;
    request.atlas = &atlas;
    request.ticket_id = 77U;
    request.expected_readback_checksum = oracle.checksum;
    request.flags = kNativeShaderExecutionReadback |
        kNativeShaderExecutionRequireExactReadback;
    ShaderReadback gpu_readback;
    NativeShaderExecutionReceipt receipt;
    assert(executor->execute(request, &gpu_readback, &receipt, &shader_error));
    assert(receipt.api_kind == kind);
    assert(receipt.command_count == 68U);
    assert(receipt.fill_instance_count == 65U);
    assert(receipt.glyph_instance_count == 240U);
    assert(receipt.atlas_binding_count == 3U);
    assert(receipt.output_bytes == 921'600U);
    assert(receipt.readback_checksum == oracle.checksum);
    assert(gpu_readback.checksum == oracle.checksum);
    assert(gpu_readback.bgra == oracle.bgra);
    assert(executor->retire_completed(receipt.signal_fence_value, &shader_error));

    const NativeShaderExecutionSnapshot snapshot = executor->snapshot();
    assert(snapshot.executions == 1U);
    assert(snapshot.readbacks == 1U);
    assert(snapshot.resident_atlas_pages == 3U);
    assert(snapshot.context.device_generation == 31U);
    assert(snapshot.context.runtime_generation == 41U);
    executor->shutdown();
    return 0;
}
