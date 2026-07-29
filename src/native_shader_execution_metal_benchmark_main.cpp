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
#include <numeric>
#include <vector>

namespace {
using namespace zevryon::text;
using namespace zevryon::text::test;

double percentile(std::vector<double> values, double ratio) {
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        ratio * static_cast<double>(values.size() - 1U));
    return values[index];
}

NativeGpuSdkConfig make_sdk_config(
    const NativeWindowSurfaceHandle& window) {
    NativeGpuSdkConfig config{};
    config.api_kind = NativeGpuApiKind::Metal;
    config.allow_software_device = 0U;
    config.require_real_device = 1U;
    config.device_generation = 401U;
    config.runtime_generation = 403U;
    config.limits = default_native_gpu_sdk_limits(NativeGpuApiKind::Metal);
    config.window = window;
    return config;
}

} // namespace

int main(int argc, char** argv) {
    const std::uint32_t iterations = argc > 1
        ? static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10))
        : 64U;
    if (iterations == 0U || iterations > 512U) {
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
    ShaderPacketFixture fixture = make_shader_packet_fixture();
    ShaderPacketError packet_error;
    if (!compile_gpu_shader_packet(fixture.input(), &cold, &packet_error)) {
        std::cerr << packet_error.message << '\n';
        return 3;
    }
    ShaderAtlasResidency atlas(8U, 1U << 20U);
    if (!atlas.apply_packet_uploads(cold, &packet_error)) {
        std::cerr << packet_error.message << '\n';
        return 4;
    }
    ShaderReadback reference;
    if (!execute_shader_packet_reference(cold, atlas, &reference, &packet_error)) {
        std::cerr << packet_error.message << '\n';
        return 5;
    }
    fixture.uploads.clear();
    fixture.payload.clear();
    if (!compile_gpu_shader_packet(fixture.input(2U, 7U), &hot, &packet_error)) {
        std::cerr << packet_error.message << '\n';
        return 6;
    }

    std::unique_ptr<MetalWindowTestHost> host =
        make_metal_window_test_host(640U, 360U);
    if (host == nullptr) {
        return 7;
    }
    host->set_visible(true);
    host->pump_events();

    std::unique_ptr<NativeGpuSdkApi> owner =
        make_metal_window_native_gpu_sdk_api();
    NativeGpuSdkError sdk_error;
    if (owner == nullptr ||
        !owner->initialize(make_sdk_config(host->handle()), &sdk_error)) {
        std::cerr << sdk_error.message << '\n';
        return 8;
    }
    NativeGpuSdkContextHandle context;
    if (!owner->export_context(&context, &sdk_error)) {
        std::cerr << sdk_error.message << '\n';
        return 9;
    }
    std::unique_ptr<NativeShaderExecutor> executor =
        make_metal_native_shader_executor();
    NativeShaderExecutionConfig config{};
    config.context = context;
    config.limits = default_native_shader_execution_limits(
        NativeGpuApiKind::Metal);
    config.executor_generation = 407U;
    NativeShaderExecutionError error;
    if (executor == nullptr || !executor->configure(config, &error)) {
        std::cerr << error.message << '\n';
        return 10;
    }
    ShaderReadback warmup;
    if (!executor->execute(cold, atlas, &warmup, &error) ||
        warmup.checksum != reference.checksum ||
        warmup.bgra != reference.bgra) {
        std::cerr << error.message << '\n';
        return 11;
    }
    owner->shutdown();
    owner.reset();

    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::uint32_t index = 0U; index < iterations; ++index) {
        const auto begin = std::chrono::steady_clock::now();
        ShaderReadback readback;
        if (!executor->execute(hot, atlas, &readback, &error) ||
            readback.checksum != reference.checksum ||
            readback.bgra != reference.bgra) {
            std::cerr << error.message << '\n';
            return 12;
        }
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(
            end - begin).count());
    }

    const NativeShaderExecutionSnapshot snapshot = executor->snapshot();
    const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
        static_cast<double>(samples.size());
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "schema=zevryon.metal-integer-shader-execution.v1\n";
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
    std::cout << "reference_checksum=" << reference.checksum << '\n';
    std::cout << "gpu_checksum=" << snapshot.last_readback_checksum << '\n';
    std::cout << "p50_ms=" << percentile(samples, 0.50) << '\n';
    std::cout << "p95_ms=" << percentile(samples, 0.95) << '\n';
    std::cout << "p99_ms=" << percentile(samples, 0.99) << '\n';
    std::cout << "max_ms=" << *std::max_element(samples.begin(), samples.end()) << '\n';
    std::cout << "mean_ms=" << mean << '\n';
    return 0;
}
