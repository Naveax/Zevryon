#include "native_shader_execution.hpp"
#include "native_vulkan_wsi.hpp"
#include "native_vulkan_wsi_test_window.hpp"
#include "shader_draw_packet_fixture.hpp"

#include <algorithm>
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

NativeGpuSdkConfig owner_config(const NativeWindowSurfaceHandle& window) {
    NativeGpuSdkConfig config{};
    config.api_kind = NativeGpuApiKind::Vulkan;
    config.allow_software_device = 1U;
    config.require_real_device = 0U;
    config.device_generation = 311U;
    config.runtime_generation = 313U;
    config.window = window;
    config.limits.maximum_swapchain_images = 4U;
    config.limits.maximum_frames_in_flight = 2U;
    config.limits.maximum_device_local_bytes = 128U * 1024U * 1024U;
    return config;
}

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        fraction * static_cast<double>(values.size() - 1U));
    return values[index];
}

bool same_readback(const ShaderReadback& left, const ShaderReadback& right) {
    return left.width == right.width && left.height == right.height &&
        left.row_bytes == right.row_bytes && left.checksum == right.checksum &&
        left.bgra == right.bgra;
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t iterations = 32U;
    if (argc > 1) {
        iterations = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
    }
    if (iterations == 0U) {
        return 2;
    }

    std::vector<std::byte> cold_storage(2U * 1024U * 1024U);
    std::vector<std::byte> hot_storage(2U * 1024U * 1024U);
    std::pmr::monotonic_buffer_resource cold_resource(
        cold_storage.data(), cold_storage.size());
    std::pmr::monotonic_buffer_resource hot_resource(
        hot_storage.data(), hot_storage.size());
    GpuShaderPacket cold(&cold_resource);
    GpuShaderPacket hot(&hot_resource);
    ShaderAtlasResidency atlas(8U, 1U << 20U);
    ShaderPacketFixture fixture = make_shader_packet_fixture();
    ShaderPacketError packet_error;
    if (!compile_gpu_shader_packet(fixture.input(), &cold, &packet_error) ||
        !atlas.apply_packet_uploads(cold, &packet_error)) {
        return 3;
    }
    ShaderReadback reference;
    if (!execute_shader_packet_reference(cold, atlas, &reference, &packet_error)) {
        return 4;
    }
    fixture.uploads.clear();
    fixture.payload.clear();
    if (!compile_gpu_shader_packet(fixture.input(2U, 7U), &hot, &packet_error)) {
        return 5;
    }

    const NativeWindowSystem system = selected_system();
    NativeVulkanTestWindow window;
    std::string window_error;
    if (!window.create(system, 640U, 360U, &window_error)) {
        return 6;
    }
    auto owner = make_vulkan_wsi_native_gpu_sdk_api();
    NativeGpuSdkError sdk_error;
    if (owner == nullptr ||
        !owner->initialize(owner_config(window.handle()), &sdk_error)) {
        return 7;
    }
    NativeGpuSdkContextHandle context;
    if (!owner->export_context(&context, &sdk_error)) {
        return 8;
    }
    const std::uint32_t software_device =
        (context.flags & kNativeGpuSdkContextSoftwareDevice) != 0U ? 1U : 0U;
    auto executor = make_vulkan_native_shader_executor();
    NativeShaderExecutionConfig config{};
    config.context = context;
    config.limits = default_native_shader_execution_limits(NativeGpuApiKind::Vulkan);
    config.executor_generation = 317U;
    NativeShaderExecutionError error;
    if (executor == nullptr || !executor->configure(config, &error)) {
        return 9;
    }
    ShaderReadback cold_readback;
    if (!executor->execute(cold, atlas, &cold_readback, &error) ||
        !same_readback(cold_readback, reference)) {
        return 10;
    }
    owner->shutdown();
    owner.reset();

    std::vector<double> samples;
    samples.reserve(iterations);
    std::uint64_t checksum = 0U;
    for (std::uint32_t index = 0U; index < iterations; ++index) {
        const auto start = std::chrono::steady_clock::now();
        ShaderReadback readback;
        if (!executor->execute(hot, atlas, &readback, &error) ||
            !same_readback(readback, reference)) {
            return 11;
        }
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(
            end - start).count());
        checksum ^= readback.checksum + static_cast<std::uint64_t>(index);
    }
    const NativeShaderExecutionSnapshot snapshot = executor->snapshot();
    executor->shutdown();
    window.destroy();

    const double average = std::accumulate(
        samples.begin(), samples.end(), 0.0) /
        static_cast<double>(samples.size());
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "schema=zevryon.vulkan-integer-shader-execution.v1\n";
    std::cout << "iterations=" << iterations << '\n';
    std::cout << "samples=" << samples.size() << '\n';
    std::cout << "commands=" << hot.header.command_count << '\n';
    std::cout << "fill_instances=" << hot.header.fill_instance_count << '\n';
    std::cout << "glyph_instances=" << hot.header.glyph_instance_count << '\n';
    std::cout << "resident_pages=" << atlas.resident_pages() << '\n';
    std::cout << "resident_atlas_bytes=" << atlas.resident_bytes() << '\n';
    std::cout << "output_surface_bytes=" << snapshot.output_surface_bytes << '\n';
    std::cout << "atlas_upload_batches=" << snapshot.atlas_upload_batches << '\n';
    std::cout << "atlas_reuses=" << snapshot.atlas_reuses << '\n';
    std::cout << "software_device=" << software_device << '\n';
    std::cout << "reference_checksum=" << reference.checksum << '\n';
    std::cout << "gpu_checksum=" << snapshot.last_readback_checksum << '\n';
    std::cout << "aggregate_checksum=" << checksum << '\n';
    std::cout << "p50_ms=" << percentile(samples, 0.50) << '\n';
    std::cout << "p95_ms=" << percentile(samples, 0.95) << '\n';
    std::cout << "p99_ms=" << percentile(samples, 0.99) << '\n';
    std::cout << "max_ms=" << *std::max_element(samples.begin(), samples.end()) << '\n';
    std::cout << "average_ms=" << average << '\n';
    return 0;
}
