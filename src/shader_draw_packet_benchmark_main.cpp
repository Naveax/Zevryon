#include "shader_draw_packet.hpp"
#include "shader_draw_packet_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory_resource>
#include <numeric>
#include <vector>

namespace {
using namespace zevryon::text;
using namespace zevryon::text::test;

using Clock = std::chrono::steady_clock;

std::uint64_t parse_iterations(int argc, char** argv) {
    if (argc < 2) {
        return 512U;
    }
    const unsigned long long parsed = std::strtoull(argv[1], nullptr, 10);
    return parsed == 0U ? 512U : static_cast<std::uint64_t>(parsed);
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double scaled = fraction * static_cast<double>(sorted.size() - 1U);
    const std::size_t index = static_cast<std::size_t>(scaled);
    return sorted[index];
}

} // namespace

int main(int argc, char** argv) {
    const std::uint64_t iterations = parse_iterations(argc, argv);
    ShaderPacketFixture cold_fixture = make_shader_packet_fixture();
    ShaderPacketFixture hot_fixture = cold_fixture;
    hot_fixture.uploads.clear();
    hot_fixture.payload.clear();

    std::vector<std::byte> cold_storage(1U << 20U);
    std::pmr::monotonic_buffer_resource cold_resource(
        cold_storage.data(), cold_storage.size());
    GpuShaderPacket cold_packet(&cold_resource);
    ShaderPacketError error;
    if (!compile_gpu_shader_packet(cold_fixture.input(), &cold_packet, &error)) {
        std::cerr << "cold packet compile failed: kind="
                  << static_cast<unsigned>(error.kind)
                  << " message=" << error.message << '\n';
        return 1;
    }
    ShaderAtlasResidency atlas(8U, 1U << 20U);
    if (!atlas.apply_packet_uploads(cold_packet, &error)) {
        std::cerr << "cold atlas publication failed: kind="
                  << static_cast<unsigned>(error.kind)
                  << " message=" << error.message << '\n';
        return 1;
    }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    std::vector<std::byte> hot_storage(1U << 17U);
    std::uint64_t checksum_accumulator = 0U;
    std::uint64_t hot_packet_bytes = 0U;
    std::uint64_t hot_packet_checksum = 0U;

    for (std::uint64_t iteration = 0U; iteration < iterations; ++iteration) {
        std::pmr::monotonic_buffer_resource resource(
            hot_storage.data(), hot_storage.size());
        GpuShaderPacket packet(&resource);
        const auto start = Clock::now();
        const bool compiled = compile_gpu_shader_packet(
            hot_fixture.input(2U, 7U), &packet, &error);
        const auto stop = Clock::now();
        if (!compiled) {
            std::cerr << "hot packet compile failed at iteration " << iteration
                      << ": kind=" << static_cast<unsigned>(error.kind)
                      << " message=" << error.message << '\n';
            return 1;
        }
        atlas.mark_packet_pages_used(packet);
        hot_packet_bytes = packet.header.packet_bytes;
        hot_packet_checksum = packet.header.packet_checksum;
        checksum_accumulator ^= packet.header.packet_checksum + iteration;
        samples.push_back(
            std::chrono::duration<double, std::milli>(stop - start).count());
    }

    std::sort(samples.begin(), samples.end());
    const double total = std::accumulate(samples.begin(), samples.end(), 0.0);
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "iterations=" << iterations << '\n';
    std::cout << "samples=" << samples.size() << '\n';
    std::cout << "commands=" << cold_packet.header.command_count << '\n';
    std::cout << "scissors=" << cold_packet.header.scissor_count << '\n';
    std::cout << "fill_instances=" << cold_packet.header.fill_instance_count << '\n';
    std::cout << "glyph_instances=" << cold_packet.header.glyph_instance_count << '\n';
    std::cout << "cold_uploads=" << cold_packet.header.upload_count << '\n';
    std::cout << "cold_payload_bytes=" << cold_packet.header.upload_payload_bytes << '\n';
    std::cout << "cold_packet_bytes=" << cold_packet.header.packet_bytes << '\n';
    std::cout << "hot_packet_bytes=" << hot_packet_bytes << '\n';
    std::cout << "resident_pages=" << atlas.resident_pages() << '\n';
    std::cout << "resident_atlas_bytes=" << atlas.resident_bytes() << '\n';
    std::cout << "cold_packet_checksum=" << cold_packet.header.packet_checksum << '\n';
    std::cout << "hot_packet_checksum=" << hot_packet_checksum << '\n';
    std::cout << "checksum_accumulator=" << checksum_accumulator << '\n';
    std::cout << "mean_ms=" << total / static_cast<double>(samples.size()) << '\n';
    std::cout << "p50_ms=" << percentile(samples, 0.50) << '\n';
    std::cout << "p95_ms=" << percentile(samples, 0.95) << '\n';
    std::cout << "p99_ms=" << percentile(samples, 0.99) << '\n';
    std::cout << "max_ms=" << samples.back() << '\n';
    return 0;
}
