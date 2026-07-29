#include "native_metal_window.hpp"
#include "native_metal_window_test_window.hpp"
#include "native_shader_execution.hpp"
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
#include <vector>

namespace {
using namespace zevryon::text;
using namespace zevryon::text::test;

struct PacketSet final {
    std::vector<std::byte> cold_storage{2U * 1024U * 1024U};
    std::vector<std::byte> hot_storage{2U * 1024U * 1024U};
    std::pmr::monotonic_buffer_resource cold_resource{
        cold_storage.data(), cold_storage.size()};
    std::pmr::monotonic_buffer_resource hot_resource{
        hot_storage.data(), hot_storage.size()};
    GpuShaderPacket cold{&cold_resource};
    GpuShaderPacket hot{&hot_resource};
    ShaderAtlasResidency atlas{8U, 1U << 20U};
    ShaderReadback reference;
};

bool build_packets(PacketSet* output) {
    ShaderPacketFixture fixture = make_shader_packet_fixture();
    ShaderPacketError error;
    if (output == nullptr ||
        !compile_gpu_shader_packet(fixture.input(), &output->cold, &error) ||
        !output->atlas.apply_packet_uploads(output->cold, &error) ||
        !execute_shader_packet_reference(
            output->cold, output->atlas, &output->reference, &error)) {
        return false;
    }
    fixture.uploads.clear();
    fixture.payload.clear();
    return compile_gpu_shader_packet(
        fixture.input(2U, 7U), &output->hot, &error);
}

NativeGpuSdkConfig owner_config(const NativeWindowSurfaceHandle& window) {
    NativeGpuSdkConfig config{};
    config.api_kind = NativeGpuApiKind::Metal;
    config.require_real_device = 1U;
    config.device_generation = 331U;
    config.runtime_generation = 337U;
    config.window = window;
    config.limits.maximum_swapchain_images = 4U;
    config.limits.maximum_frames_in_flight = 2U;
    config.limits.maximum_device_local_bytes = 128U * 1024U * 1024U;
    return config;
}

double percentile(const std::vector<double>& values, double fraction) {
    const std::size_t index = static_cast<std::size_t>(
        fraction * static_cast<double>(values.size() - 1U));
    return values[index];
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t iterations = 64U;
    if (argc > 1) {
        iterations = static_cast<std::uint32_t>(
            std::strtoul(argv[1], nullptr, 10));
    }
    if (iterations == 0U) {
        return 1;
    }

    PacketSet packets;
    if (!build_packets(&packets)) {
        return 2;
    }
    std::unique_ptr<MetalWindowTestHost> window =
        make_metal_window_test_host(640U, 360U);
    if (window == nullptr) {
        return 3;
    }
    window->set_visible(true);
    window->pump_events();

    std::unique_ptr<NativeGpuSdkApi> owner =
        make_metal_window_native_gpu_sdk_api();
    NativeGpuSdkError sdk_error;
    if (owner == nullptr ||
        !owner->initialize(owner_config(window->handle()), &sdk_error)) {
        return 4;
    }
    NativeGpuSdkContextHandle context;
    if (!owner->export_context(&context, &sdk_error)) {
        return 5;
    }
    std::unique_ptr<NativeShaderExecutor> executor =
        make_metal_native_shader_executor();
    NativeShaderExecutionConfig config{};
    config.context = context;
    config.limits = default_native_shader_execution_limits(
        NativeGpuApiKind::Metal);
    config.executor_generation = 347U;
    NativeShaderExecutionError error;
    if (executor == nullptr || !executor->configure(config, &error)) {
        return 6;
    }

    ShaderReadback cold;
    if (!executor->execute(packets.cold, packets.atlas, &cold, &error) ||
        cold.checksum != packets.reference.checksum) {
        return 7;
    }
    owner->shutdown();
    owner.reset();

    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::uint32_t index = 0U; index < iterations; ++index) {
        const auto start = std::chrono::steady_clock::now();
        ShaderReadback readback;
        if (!executor->execute(
                packets.hot, packets.atlas, &readback, &error) ||
            readback.checksum != packets.reference.checksum) {
            return 8;
        }
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(
            end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    double mean = 0.0;
    for (const double value : samples) {
        mean += value;
    }
    mean /= static_cast<double>(samples.size());
    const NativeShaderExecutionSnapshot snapshot = executor->snapshot();

    std::cout << std::fixed << std::setprecision(6)
              << "schema=zevryon.metal-integer-shader-execution.v1\n"
              << "iterations=" << iterations << '\n'
              << "samples=" << samples.size() << '\n'
              << "commands=" << packets.hot.header.command_count << '\n'
              << "fill_instances=" << packets.hot.header.fill_instance_count << '\n'
              << "glyph_instances=" << packets.hot.header.glyph_instance_count << '\n'
              << "resident_pages=" << packets.atlas.resident_pages() << '\n'
              << "resident_atlas_bytes=" << packets.atlas.resident_bytes() << '\n'
              << "output_surface_bytes=" << snapshot.output_surface_bytes << '\n'
              << "atlas_upload_batches=" << snapshot.atlas_upload_batches << '\n'
              << "atlas_reuses=" << snapshot.atlas_reuses << '\n'
              << "reference_checksum=" << packets.reference.checksum << '\n'
              << "gpu_checksum=" << snapshot.last_readback_checksum << '\n'
              << "p50_ms=" << percentile(samples, 0.50) << '\n'
              << "p95_ms=" << percentile(samples, 0.95) << '\n'
              << "p99_ms=" << percentile(samples, 0.99) << '\n'
              << "max_ms=" << samples.back() << '\n'
              << "mean_ms=" << mean << '\n';
    executor->shutdown();
    window.reset();
    return 0;
}
