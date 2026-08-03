#include "native_shader_execution.hpp"
#include "shader_draw_packet_fixture.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <vector>

namespace {
using namespace zevryon::text;
using namespace zevryon::text::test;

struct PacketSet final {
    std::vector<std::byte> cold_storage;
    std::vector<std::byte> hot_storage;
    std::pmr::monotonic_buffer_resource cold_resource;
    std::pmr::monotonic_buffer_resource hot_resource;
    GpuShaderPacket cold;
    GpuShaderPacket hot;
    ShaderAtlasResidency atlas;
    ShaderReadback reference;

    PacketSet()
        : cold_storage(2U * 1024U * 1024U),
          hot_storage(2U * 1024U * 1024U),
          cold_resource(cold_storage.data(), cold_storage.size()),
          hot_resource(hot_storage.data(), hot_storage.size()),
          cold(&cold_resource),
          hot(&hot_resource),
          atlas(8U, 1U << 20U) {}
};

bool build_packets(PacketSet* output) {
    if (output == nullptr) {
        return false;
    }
    ShaderPacketFixture fixture = make_shader_packet_fixture();
    ShaderPacketError error;
    if (!compile_gpu_shader_packet(fixture.input(), &output->cold, &error) ||
        !output->atlas.apply_packet_uploads(output->cold, &error) ||
        !execute_shader_packet_reference(
            output->cold, output->atlas, &output->reference, &error)) {
        std::cerr << "packet preparation failed: " << error.message << '\n';
        return false;
    }
    fixture.uploads.clear();
    fixture.payload.clear();
    if (!compile_gpu_shader_packet(
            fixture.input(2U, 7U), &output->hot, &error)) {
        std::cerr << "hot packet preparation failed: " << error.message << '\n';
        return false;
    }
    return true;
}

NativeGpuSdkConfig make_sdk_config() {
    NativeGpuSdkConfig config{};
    config.api_kind = NativeGpuApiKind::Direct3D12;
    config.allow_software_device = 1U;
    config.require_real_device = 0U;
    config.enable_validation = 0U;
    config.device_generation = 101U;
    config.runtime_generation = 103U;
    config.limits = default_native_gpu_sdk_limits(NativeGpuApiKind::Direct3D12);
    config.window.system = NativeWindowSystem::Headless;
    config.window.generation = 107U;
    return config;
}

bool readbacks_equal(const ShaderReadback& left, const ShaderReadback& right) {
    return left.width == right.width && left.height == right.height &&
        left.row_bytes == right.row_bytes && left.checksum == right.checksum &&
        left.bgra.size() == right.bgra.size() &&
        std::equal(left.bgra.begin(), left.bgra.end(), right.bgra.begin());
}

} // namespace

int main() {
    assert(native_shader_execution_build_has_backend(
        NativeGpuApiKind::Direct3D12));
    PacketSet packets;
    assert(build_packets(&packets));
    assert(packets.cold.header.command_count == 68U);
    assert(packets.cold.header.fill_instance_count == 65U);
    assert(packets.cold.header.glyph_instance_count == 240U);
    assert(packets.atlas.resident_pages() == 3U);
    assert(packets.atlas.resident_bytes() == 49'152U);

    std::unique_ptr<NativeGpuSdkApi> owner =
        make_direct3d12_native_gpu_sdk_api();
    assert(owner != nullptr);
    NativeGpuSdkError sdk_error;
    assert(owner->initialize(make_sdk_config(), &sdk_error));
    NativeGpuSdkContextHandle context;
    assert(owner->export_context(&context, &sdk_error));

    std::unique_ptr<NativeShaderExecutor> executor =
        make_direct3d12_native_shader_executor();
    assert(executor != nullptr);
    NativeShaderExecutionConfig config{};
    config.context = context;
    config.limits = default_native_shader_execution_limits(
        NativeGpuApiKind::Direct3D12);
    config.executor_generation = 109U;
    NativeShaderExecutionError error;
    assert(executor->configure(config, &error));

    ShaderReadback cold_readback;
    assert(executor->execute(
        packets.cold, packets.atlas, &cold_readback, &error));
    assert(readbacks_equal(cold_readback, packets.reference));

    owner->shutdown();
    owner.reset();

    ShaderReadback hot_readback;
    assert(executor->execute(
        packets.hot, packets.atlas, &hot_readback, &error));
    assert(readbacks_equal(hot_readback, packets.reference));

    // The production path executes the same certified packet without copying
    // the full output surface back to CPU memory.
    assert(executor->execute(packets.hot, packets.atlas, nullptr, &error));
    NativeShaderSurfaceView surface;
    assert(executor->export_surface(&surface, &error));
    assert(native_shader_surface_view_valid(surface));
    assert(surface.api_kind == NativeGpuApiKind::Direct3D12);
    assert(surface.format == GpuSurfaceFormat::Bgra8Unorm);
    assert(surface.state == NativeShaderSurfaceState::ShaderRead);
    assert(surface.device_generation == context.device_generation);
    assert(surface.runtime_generation == context.runtime_generation);
    assert(surface.executor_generation == config.executor_generation);
    assert(surface.output_generation != 0U);
    assert(surface.frame_id == packets.hot.header.frame_id);
    assert(surface.content_checksum == packets.hot.header.packet_checksum);
    assert(surface.native_resource != 0U);
    assert(surface.width == packets.hot.header.surface_width);
    assert(surface.height == packets.hot.header.surface_height);

    NativeShaderExecutionSnapshot snapshot = executor->snapshot();
    assert(snapshot.configured == 1U);
    assert(snapshot.executions == 3U);
    assert(snapshot.readbacks == 2U);
    assert(snapshot.atlas_upload_batches == 1U);
    assert(snapshot.atlas_reuses == 2U);
    assert(snapshot.persistent_atlas_bytes == 49'152U);
    assert(snapshot.output_surface_bytes == 921'600U);
    assert(snapshot.last_readback_checksum == packets.reference.checksum);
    assert((snapshot.capability_flags &
        kNativeShaderExecutionRetainedContext) != 0U);
    assert((snapshot.capability_flags &
        kNativeShaderExecutionDirectSurfaceExport) != 0U);

    GpuShaderPacket corrupted(&packets.hot_resource);
    corrupted.header = packets.hot.header;
    corrupted.header.packet_checksum ^= 1U;
    ShaderReadback rejected;
    assert(!executor->execute(corrupted, packets.atlas, &rejected, &error));
    assert(error.kind == NativeShaderExecutionErrorKind::InvalidInput);

    executor->shutdown();
    NativeShaderSurfaceView shutdown_surface;
    assert(!executor->export_surface(&shutdown_surface, &error));
    assert(error.kind == NativeShaderExecutionErrorKind::StaleGeneration);

    std::cout << "real D3D12 integer shader execution: commands="
              << packets.hot.header.command_count
              << " fills=" << packets.hot.header.fill_instance_count
              << " glyphs=" << packets.hot.header.glyph_instance_count
              << " checksum=" << packets.reference.checksum
              << " PASS\n";
    return 0;
}
