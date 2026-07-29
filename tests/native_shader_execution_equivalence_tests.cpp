#include "native_shader_execution.hpp"
#include "shader_draw_packet_fixture.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

using namespace zevryon::text;
using namespace zevryon::text::test;

int main() {
    const std::array<NativeGpuApiKind, 4U> backends{
        NativeGpuApiKind::ReferenceCpu,
        NativeGpuApiKind::Direct3D12,
        NativeGpuApiKind::Vulkan,
        NativeGpuApiKind::Metal};
    std::uint64_t passed = 0U;
    for (std::uint32_t mask = 0U; mask < 1024U; ++mask) {
        ShaderPacketFixture fixture = make_shader_packet_fixture();
        for (std::uint32_t bit = 0U; bit < 10U; ++bit) {
            if ((mask & (1U << bit)) != 0U) {
                ShaderColorBgra8& color = fixture.fills[bit].color;
                color.blue = static_cast<std::uint8_t>(color.blue ^ (bit * 7U + 1U));
                color.green = static_cast<std::uint8_t>(color.green ^ (bit * 5U + 3U));
            }
        }
        std::vector<std::byte> storage(2U * 1024U * 1024U);
        std::pmr::monotonic_buffer_resource arena(
            storage.data(), storage.size(), std::pmr::null_memory_resource());
        GpuShaderPacket packet(&arena);
        ShaderPacketError packet_error;
        assert(compile_gpu_shader_packet(
            fixture.input(static_cast<std::uint64_t>(mask) + 1U, 7U),
            &packet, &packet_error));
        ShaderAtlasResidency atlas(8U, 1U << 20U);
        assert(atlas.apply_packet_uploads(packet, &packet_error));
        ShaderReadback oracle;
        assert(execute_shader_packet_reference(packet, atlas, &oracle, &packet_error));

        std::uint64_t stable_bytes = 0U;
        for (const NativeGpuApiKind backend : backends) {
            NativeShaderDispatchPlan plan;
            NativeShaderExecutionError error;
            assert(compile_native_shader_dispatch_plan(
                backend, packet, atlas,
                default_native_shader_execution_limits(), &plan, &error));
            const std::uint64_t bytes =
                plan.header.command_bytes + plan.header.fill_bytes +
                plan.header.glyph_bytes + plan.header.scissor_bytes +
                plan.header.atlas_bytes + plan.header.output_bytes;
            if (stable_bytes == 0U) {
                stable_bytes = bytes;
            }
            assert(bytes == stable_bytes);
            assert(plan.header.dispatch_x == 80U);
            assert(plan.header.dispatch_y == 45U);
            assert(plan.header.atlas_binding_count == 3U);
            assert(plan.header.packet_checksum == packet.header.packet_checksum);
            assert(plan.header.plan_checksum ==
                native_shader_dispatch_plan_checksum(plan));
            assert(oracle.checksum != 0U);
            ++passed;
        }
    }
    assert(passed == 4'096U);
    return 0;
}
