#include "native_shader_execution.hpp"
#include "shader_draw_packet_fixture.hpp"

#if defined(ZEVRYON_NATIVE_SHADER_TEST_METAL)
#include "native_metal_window.hpp"
#include "native_metal_window_test_window.hpp"
#elif defined(ZEVRYON_NATIVE_SHADER_TEST_VULKAN)
#include "native_vulkan_wsi.hpp"
#include "native_vulkan_wsi_test_window.hpp"
#endif

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <numeric>
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

const char* backend_name() noexcept {
#if defined(ZEVRYON_NATIVE_SHADER_TEST_D3D12)
    return "d3d12";
#elif defined(ZEVRYON_NATIVE_SHADER_TEST_VULKAN)
    return "vulkan";
#elif defined(ZEVRYON_NATIVE_SHADER_TEST_METAL)
    return "metal";
#else
    return "reference";
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

int main(int argc, char** argv) {
    std::uint32_t iterations = 128U;
    if (argc > 1) {
        iterations = static_cast<std::uint32_t>(
            std::strtoul(argv[1], nullptr, 10));
    }
    assert(iterations != 0U);

    const NativeGpuApiKind kind = backend_kind();
    std::unique_ptr<NativeGpuSdkApi> owner = make_owner();
    std::unique_ptr<NativeShaderExecutionApi> executor = make_executor();
    assert(owner != nullptr);
    assert(executor != nullptr);

    NativeGpuSdkConfig config;
    config.api_kind = kind;
    config.device_generation = 131U;
    config.runtime_generation = 141U;
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
    assert(executor->configure(
        context, default_native_shader_execution_limits(), &shader_error));
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

    std::vector<double> samples;
    samples.reserve(iterations);
    std::uint64_t stable_plan_checksum = 0U;
    std::uint64_t stable_readback_checksum = 0U;
    for (std::uint32_t iteration = 0U; iteration < iterations; ++iteration) {
        NativeShaderExecutionRequest request;
        request.packet = &packet;
        request.atlas = &atlas;
        request.ticket_id = static_cast<std::uint64_t>(iteration) + 1U;
        request.expected_readback_checksum = oracle.checksum;
        request.flags = kNativeShaderExecutionReadback |
            kNativeShaderExecutionRequireExactReadback;
        ShaderReadback gpu_readback;
        NativeShaderExecutionReceipt receipt;
        const auto start = std::chrono::steady_clock::now();
        assert(executor->execute(
            request, &gpu_readback, &receipt, &shader_error));
        assert(executor->retire_completed(
            receipt.signal_fence_value, &shader_error));
        const auto stop = std::chrono::steady_clock::now();
        assert(gpu_readback.bgra == oracle.bgra);
        if (stable_plan_checksum == 0U) {
            stable_plan_checksum = receipt.plan_checksum;
            stable_readback_checksum = receipt.readback_checksum;
        }
        assert(receipt.plan_checksum == stable_plan_checksum);
        assert(receipt.readback_checksum == stable_readback_checksum);
        samples.push_back(std::chrono::duration<double, std::milli>(
            stop - start).count());
    }

    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double value) {
        const std::size_t index = static_cast<std::size_t>(
            value * static_cast<double>(samples.size() - 1U));
        return samples[index];
    };
    const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
        static_cast<double>(samples.size());
    const NativeShaderExecutionSnapshot snapshot = executor->snapshot();

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "schema=zevryon.native-shader-platform.v1\n";
    std::cout << "backend=" << backend_name() << "\n";
    std::cout << "iterations=" << iterations << "\n";
    std::cout << "samples=" << samples.size() << "\n";
    std::cout << "commands=" << packet.commands.size() << "\n";
    std::cout << "fills=" << packet.fills.size() << "\n";
    std::cout << "glyphs=" << packet.glyphs.size() << "\n";
    std::cout << "scissors=" << packet.scissors.size() << "\n";
    std::cout << "atlas_pages=" << atlas.resident_pages() << "\n";
    std::cout << "atlas_bytes=" << atlas.resident_bytes() << "\n";
    std::cout << "output_bytes=" << oracle.bgra.size() << "\n";
    std::cout << "readback_checksum=" << stable_readback_checksum << "\n";
    std::cout << "plan_checksum=" << stable_plan_checksum << "\n";
    std::cout << "executions=" << snapshot.executions << "\n";
    std::cout << "readbacks=" << snapshot.readbacks << "\n";
    std::cout << "peak_device_bytes=" << snapshot.peak_device_bytes << "\n";
    std::cout << "peak_staging_bytes=" << snapshot.peak_staging_bytes << "\n";
    std::cout << "p50_ms=" << percentile(0.50) << "\n";
    std::cout << "p95_ms=" << percentile(0.95) << "\n";
    std::cout << "p99_ms=" << percentile(0.99) << "\n";
    std::cout << "max_ms=" << samples.back() << "\n";
    std::cout << "mean_ms=" << mean << "\n";
    executor->shutdown();
    return 0;
}
