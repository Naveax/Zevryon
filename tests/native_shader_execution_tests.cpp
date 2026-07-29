#include "native_shader_execution.hpp"
#include "shader_draw_packet_fixture.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

namespace {
using namespace zevryon::text;
using namespace zevryon::text::test;

NativeGpuSdkContextHandle reference_context() {
    NativeGpuSdkContextHandle context;
    context.api_kind = NativeGpuApiKind::ReferenceCpu;
    context.device_generation = 1U;
    context.runtime_generation = 1U;
    return context;
}

void test_exact_plan_and_reference_execution() {
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
    assert(oracle.width == 640U);
    assert(oracle.height == 360U);
    assert(oracle.row_bytes == 2560U);
    assert(oracle.bgra.size() == 921'600U);

    const NativeShaderExecutionLimits limits =
        default_native_shader_execution_limits();
    for (const NativeGpuApiKind kind : {
             NativeGpuApiKind::ReferenceCpu,
             NativeGpuApiKind::Direct3D12,
             NativeGpuApiKind::Vulkan,
             NativeGpuApiKind::Metal}) {
        NativeShaderDispatchPlan plan;
        NativeShaderExecutionError error;
        assert(compile_native_shader_dispatch_plan(
            kind, packet, atlas, limits, &plan, &error));
        assert(plan.header.api_kind == kind);
        assert(plan.header.dispatch_x == 80U);
        assert(plan.header.dispatch_y == 45U);
        assert(plan.header.dispatch_z == 1U);
        assert(plan.header.atlas_binding_count == 3U);
        assert(plan.header.command_bytes == 3'264U);
        assert(plan.header.fill_bytes == 2'080U);
        assert(plan.header.glyph_bytes == 15'360U);
        assert(plan.header.scissor_bytes == 16U);
        assert(plan.header.atlas_bytes == 49'152U);
        assert(plan.header.output_bytes == 921'600U);
        assert(plan.header.packet_checksum == packet.header.packet_checksum);
        assert(plan.header.plan_checksum ==
            native_shader_dispatch_plan_checksum(plan));
        assert(plan.constants.surface_width == 640U);
        assert(plan.constants.surface_height == 360U);
        assert(plan.constants.command_count == 68U);
        assert(plan.constants.atlas_layer_count == 3U);
        assert(plan.atlas_bindings.size() == 3U);
        for (std::size_t index = 0U; index < plan.atlas_bindings.size(); ++index) {
            assert(plan.atlas_bindings[index].page_index == index);
            assert(plan.atlas_bindings[index].texture_layer == index);
            assert(plan.atlas_bindings[index].width == 64U);
            assert(plan.atlas_bindings[index].height == 64U);
            assert(plan.atlas_bindings[index].resident_bytes == 16'384U);
        }
    }

    auto executor = make_reference_native_shader_execution_api();
    assert(executor != nullptr);
    NativeShaderExecutionError error;
    assert(executor->configure(reference_context(), limits, &error));
    NativeShaderExecutionRequest request;
    request.packet = &packet;
    request.atlas = &atlas;
    request.ticket_id = 17U;
    request.expected_readback_checksum = oracle.checksum;
    request.flags = kNativeShaderExecutionReadback |
        kNativeShaderExecutionRequireExactReadback;
    ShaderReadback readback;
    NativeShaderExecutionReceipt receipt;
    assert(executor->execute(request, &readback, &receipt, &error));
    assert(readback.checksum == oracle.checksum);
    assert(readback.bgra == oracle.bgra);
    assert(receipt.command_count == 68U);
    assert(receipt.fill_instance_count == 65U);
    assert(receipt.glyph_instance_count == 240U);
    assert(receipt.atlas_binding_count == 3U);
    assert(receipt.output_bytes == 921'600U);
    assert(receipt.readback_checksum == oracle.checksum);
    assert(executor->retire_completed(receipt.signal_fence_value, &error));
    const NativeShaderExecutionSnapshot snapshot = executor->snapshot();
    assert(snapshot.executions == 1U);
    assert(snapshot.readbacks == 1U);
    assert(snapshot.in_flight_count == 0U);
    assert(snapshot.resident_atlas_pages == 3U);
}

void test_fail_closed_paths() {
    ShaderPacketFixture fixture = make_shader_packet_fixture();
    std::vector<std::byte> storage(2U * 1024U * 1024U);
    std::pmr::monotonic_buffer_resource arena(
        storage.data(), storage.size(), std::pmr::null_memory_resource());
    GpuShaderPacket packet(&arena);
    ShaderPacketError packet_error;
    assert(compile_gpu_shader_packet(fixture.input(), &packet, &packet_error));
    ShaderAtlasResidency atlas(8U, 1U << 20U);
    assert(atlas.apply_packet_uploads(packet, &packet_error));

    NativeShaderExecutionLimits limits = default_native_shader_execution_limits();
    NativeShaderDispatchPlan sentinel;
    sentinel.header.plan_checksum = 99U;
    NativeShaderExecutionError error;

    limits.maximum_glyph_instances = 16U;
    assert(!compile_native_shader_dispatch_plan(
        NativeGpuApiKind::Vulkan, packet, atlas, limits, &sentinel, &error));
    assert(error.kind == NativeShaderExecutionErrorKind::ResourceBudgetExceeded);
    assert(sentinel.header.plan_checksum == 99U);

    limits = default_native_shader_execution_limits();
    packet.header.packet_checksum ^= 1U;
    assert(!compile_native_shader_dispatch_plan(
        NativeGpuApiKind::Metal, packet, atlas, limits, &sentinel, &error));
    assert(error.kind == NativeShaderExecutionErrorKind::InvalidPacket);
    assert(sentinel.header.plan_checksum == 99U);
}

} // namespace

int main() {
    test_exact_plan_and_reference_execution();
    test_fail_closed_paths();
    return 0;
}
