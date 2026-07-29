#include "shader_draw_packet.hpp"
#include "shader_draw_packet_fixture.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>

namespace {
using namespace zevryon::text;
using namespace zevryon::text::test;

void certify_cold_and_hot_packets() {
    ShaderPacketFixture fixture = make_shader_packet_fixture();
    std::array<std::byte, 1U << 20U> storage{};
    std::pmr::monotonic_buffer_resource resource(storage.data(), storage.size());
    GpuShaderPacket packet(&resource);
    ShaderPacketError error;
    assert(compile_gpu_shader_packet(fixture.input(), &packet, &error));
    assert(error.kind == ShaderPacketErrorKind::None);
    assert(packet.header.command_count == 68U);
    assert(packet.header.fill_instance_count == 65U);
    assert(packet.header.glyph_instance_count == 240U);
    assert(packet.header.scissor_count == 1U);
    assert(packet.header.upload_count == 3U);
    assert(packet.header.upload_payload_bytes == 32'768U);
    assert(packet.header.packet_checksum == shader_packet_checksum(packet));

    ShaderAtlasResidency atlas(8U, 1U << 20U);
    assert(atlas.apply_packet_uploads(packet, &error));
    assert(atlas.resident_pages() == 3U);
    assert(atlas.resident_bytes() == 49'152U);

    ShaderReadback cold;
    assert(execute_shader_packet_reference(packet, atlas, &cold, &error));
    assert(cold.width == 640U);
    assert(cold.height == 360U);
    assert(cold.row_bytes == 2560U);
    assert(cold.bgra.size() == 921'600U);
    assert(cold.checksum != 0U);

    ShaderPacketFixture hot_fixture = fixture;
    hot_fixture.uploads.clear();
    hot_fixture.payload.clear();
    std::array<std::byte, 1U << 18U> hot_storage{};
    std::pmr::monotonic_buffer_resource hot_resource(
        hot_storage.data(), hot_storage.size());
    GpuShaderPacket hot_packet(&hot_resource);
    assert(compile_gpu_shader_packet(
        hot_fixture.input(2U, 7U), &hot_packet, &error));
    assert(hot_packet.header.upload_count == 0U);
    assert(hot_packet.header.upload_payload_bytes == 0U);
    assert(atlas.apply_packet_uploads(hot_packet, &error));
    atlas.mark_packet_pages_used(hot_packet);
    ShaderReadback hot;
    assert(execute_shader_packet_reference(hot_packet, atlas, &hot, &error));
    assert(hot.checksum == cold.checksum);
    assert(hot.bgra == cold.bgra);

    atlas.evict_before_frame(3U);
    assert(atlas.resident_pages() == 0U);
}

void certify_fail_closed_validation() {
    ShaderPacketFixture fixture = make_shader_packet_fixture();
    std::array<std::byte, 1U << 20U> storage{};
    std::pmr::monotonic_buffer_resource resource(storage.data(), storage.size());
    GpuShaderPacket packet(&resource);
    ShaderPacketError error;
    assert(compile_gpu_shader_packet(fixture.input(), &packet, &error));
    const std::uint64_t certified_checksum = packet.header.packet_checksum;

    ShaderPacketFixture bad_order = fixture;
    std::swap(bad_order.commands[0], bad_order.commands.back());
    assert(!compile_gpu_shader_packet(bad_order.input(), &packet, &error));
    assert(error.kind == ShaderPacketErrorKind::InvalidOrdering);
    assert(packet.header.packet_checksum == certified_checksum);

    ShaderPacketFixture bad_checksum = fixture;
    bad_checksum.payload[0] ^= std::byte{1U};
    assert(!compile_gpu_shader_packet(bad_checksum.input(), &packet, &error));
    assert(error.kind == ShaderPacketErrorKind::ChecksumMismatch);
    assert(packet.header.packet_checksum == certified_checksum);

    ShaderPacketFixture bad_budget = fixture;
    bad_budget.limits.maximum_packet_bytes = 1024U;
    assert(!compile_gpu_shader_packet(bad_budget.input(), &packet, &error));
    assert(error.kind == ShaderPacketErrorKind::ResourceBudgetExceeded);
    assert(packet.header.packet_checksum == certified_checksum);

    ShaderPacketFixture bad_generation = fixture;
    bad_generation.glyphs[0].atlas_page_generation += 99U;
    assert(!compile_gpu_shader_packet(bad_generation.input(), &packet, &error));
    assert(error.kind == ShaderPacketErrorKind::InvalidInput);
    assert(packet.header.packet_checksum == certified_checksum);
}

void certify_residency_failure_atomicity() {
    ShaderPacketFixture fixture = make_shader_packet_fixture();
    std::array<std::byte, 1U << 20U> storage{};
    std::pmr::monotonic_buffer_resource resource(storage.data(), storage.size());
    GpuShaderPacket packet(&resource);
    ShaderPacketError error;
    assert(compile_gpu_shader_packet(fixture.input(), &packet, &error));

    ShaderAtlasResidency too_small(2U, 32'768U);
    assert(!too_small.apply_packet_uploads(packet, &error));
    assert(error.kind == ShaderPacketErrorKind::ResourceBudgetExceeded);
    assert(too_small.resident_pages() == 0U);
    assert(too_small.resident_bytes() == 0U);

    ShaderAtlasResidency atlas(8U, 1U << 20U);
    assert(atlas.apply_packet_uploads(packet, &error));
    const std::uint64_t bytes = atlas.resident_bytes();
    packet.upload_payload[0] ^= std::byte{1U};
    assert(!atlas.apply_packet_uploads(packet, &error));
    assert(error.kind == ShaderPacketErrorKind::ChecksumMismatch);
    assert(atlas.resident_pages() == 3U);
    assert(atlas.resident_bytes() == bytes);
}

} // namespace

int main() {
    certify_cold_and_hot_packets();
    certify_fail_closed_validation();
    certify_residency_failure_atomicity();
    std::cout << "shader draw packet tests: PASS\n";
    return 0;
}
