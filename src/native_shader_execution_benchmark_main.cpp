#include "native_shader_execution.hpp"
#include "shader_draw_packet_fixture.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory_resource>
#include <numeric>
#include <vector>

using namespace zevryon::text;
using namespace zevryon::text::test;

int main(int argc, char** argv) {
    std::uint32_t iterations = 512U;
    if (argc > 1) {
        iterations = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
    }
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
    for (std::uint32_t iteration = 0U; iteration < iterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        NativeShaderDispatchPlan plan;
        NativeShaderExecutionError error;
        assert(compile_native_shader_dispatch_plan(
            NativeGpuApiKind::Vulkan, packet, atlas,
            default_native_shader_execution_limits(), &plan, &error));
        const auto stop = std::chrono::steady_clock::now();
        if (stable_plan_checksum == 0U) {
            stable_plan_checksum = plan.header.plan_checksum;
        }
        assert(plan.header.plan_checksum == stable_plan_checksum);
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

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "schema=zevryon.native-shader-execution.v1\n";
    std::cout << "iterations=" << iterations << "\n";
    std::cout << "samples=" << samples.size() << "\n";
    std::cout << "commands=" << packet.commands.size() << "\n";
    std::cout << "fills=" << packet.fills.size() << "\n";
    std::cout << "glyphs=" << packet.glyphs.size() << "\n";
    std::cout << "scissors=" << packet.scissors.size() << "\n";
    std::cout << "atlas_pages=" << atlas.resident_pages() << "\n";
    std::cout << "atlas_bytes=" << atlas.resident_bytes() << "\n";
    std::cout << "output_bytes=" << oracle.bgra.size() << "\n";
    std::cout << "readback_checksum=" << oracle.checksum << "\n";
    std::cout << "plan_checksum=" << stable_plan_checksum << "\n";
    std::cout << "p50_ms=" << percentile(0.50) << "\n";
    std::cout << "p95_ms=" << percentile(0.95) << "\n";
    std::cout << "p99_ms=" << percentile(0.99) << "\n";
    std::cout << "max_ms=" << samples.back() << "\n";
    std::cout << "mean_ms=" << mean << "\n";
    return 0;
}
