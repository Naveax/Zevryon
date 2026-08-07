#include "native_shader_execution.hpp"
#include "native_vulkan_wsi.hpp"
#include "native_vulkan_wsi_test_window.hpp"
#include "shader_draw_packet_fixture.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <string>
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

NativeGpuSdkConfig owner_config(
    const NativeWindowSurfaceHandle& window) {
    NativeGpuSdkConfig config{};
    config.api_kind = NativeGpuApiKind::Vulkan;
    config.allow_software_device = 1U;
    config.require_real_device = 0U;
    config.enable_validation = 0U;
    config.device_generation = 211U;
    config.runtime_generation = 223U;
    config.window = window;
    config.limits.maximum_swapchain_images = 4U;
    config.limits.maximum_frames_in_flight = 2U;
    config.limits.maximum_device_local_bytes = 128U * 1024U * 1024U;
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
    const NativeWindowSystem system = selected_system();
    assert(native_vulkan_wsi_build_has_window_system(system));
    assert(native_shader_execution_build_has_backend(NativeGpuApiKind::Vulkan));

    PacketSet packets;
    assert(build_packets(&packets));
    assert(packets.cold.header.command_count == 68U);
    assert(packets.cold.header.fill_instance_count == 65U);
    assert(packets.cold.header.glyph_instance_count == 240U);
    assert(packets.atlas.resident_pages() == 3U);
    assert(packets.atlas.resident_bytes() == 49'152U);

    NativeVulkanTestWindow window;
    std::string window_error;
    assert(window.create(system, 640U, 360U, &window_error));

    std::unique_ptr<NativeGpuSdkApi> owner =
        make_vulkan_wsi_native_gpu_sdk_api();
    assert(owner != nullptr);
    NativeGpuSdkError sdk_error;
    assert(owner->initialize(owner_config(window.handle()), &sdk_error));
    NativeGpuSdkContextHandle context;
    assert(owner->export_context(&context, &sdk_error));

    std::unique_ptr<NativeShaderExecutor> executor =
        make_vulkan_native_shader_executor();
    assert(executor != nullptr);
    NativeShaderExecutionConfig config{};
    config.context = context;
    config.limits = default_native_shader_execution_limits(
        NativeGpuApiKind::Vulkan);
    config.executor_generation = 227U;
    NativeShaderExecutionError error;
    assert(executor->configure(config, &error));

    // Direct-first execution must not require a host-visible readback buffer.
    assert(executor->execute(
        packets.cold, packets.atlas, nullptr, &error));
    NativeShaderSurfaceView direct_first_surface;
    assert(executor->export_surface(&direct_first_surface, &error));
    assert(native_shader_surface_view_valid(direct_first_surface));

    NativeShaderExecutionSnapshot snapshot = executor->snapshot();
    assert(snapshot.configured == 1U);
    assert(snapshot.executions == 1U);
    assert(snapshot.readbacks == 0U);

    // A later certification readback is allocated lazily without rebuilding
    // the already-valid output image or advancing its output generation.
    ShaderReadback cold_readback;
    assert(executor->execute(
        packets.cold, packets.atlas, &cold_readback, &error));
    assert(readbacks_equal(cold_readback, packets.reference));

    NativeShaderSurfaceView after_lazy_readback;
    assert(executor->export_surface(&after_lazy_readback, &error));
    assert(after_lazy_readback.output_generation ==
        direct_first_surface.output_generation);
    assert(after_lazy_readback.native_resource ==
        direct_first_surface.native_resource);

    // Prove direct execution is independent from the readback byte budget.
    // The old eager allocation path fails this 640x360 direct execution because
    // the full-frame readback requires 921600 bytes.
    std::unique_ptr<NativeShaderExecutor> direct_only =
        make_vulkan_native_shader_executor();
    assert(direct_only != nullptr);
    NativeShaderExecutionConfig direct_only_config = config;
    direct_only_config.executor_generation = 229U;
    direct_only_config.limits.maximum_readback_bytes = 1U;
    assert(direct_only->configure(direct_only_config, &error));

    assert(direct_only->execute(
        packets.hot, packets.atlas, nullptr, &error));
    NativeShaderSurfaceView direct_only_surface;
    assert(direct_only->export_surface(&direct_only_surface, &error));
    assert(native_shader_surface_view_valid(direct_only_surface));

    NativeShaderExecutionSnapshot direct_only_snapshot =
        direct_only->snapshot();
    assert(direct_only_snapshot.executions == 1U);
    assert(direct_only_snapshot.readbacks == 0U);

    ShaderReadback denied_readback;
    assert(!direct_only->execute(
        packets.hot, packets.atlas, &denied_readback, &error));
    assert(error.kind ==
        NativeShaderExecutionErrorKind::ResourceBudgetExceeded);

    NativeShaderSurfaceView preserved_surface;
    assert(direct_only->export_surface(&preserved_surface, &error));
    assert(preserved_surface.output_generation ==
        direct_only_surface.output_generation);
    assert(preserved_surface.native_resource ==
        direct_only_surface.native_resource);

    direct_only_snapshot = direct_only->snapshot();
    assert(direct_only_snapshot.executions == 1U);
    assert(direct_only_snapshot.readbacks == 0U);
    direct_only->shutdown();

    owner->shutdown();
    owner.reset();

    ShaderReadback hot_readback;
    assert(executor->execute(
        packets.hot, packets.atlas, &hot_readback, &error));
    assert(readbacks_equal(hot_readback, packets.reference));

    snapshot = executor->snapshot();
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

    // Production direct mode must complete on the retained Vulkan image without
    // creating a new GPU-to-host readback operation.
    assert(executor->execute(
        packets.hot, packets.atlas, nullptr, &error));
    NativeShaderSurfaceView surface;
    assert(executor->export_surface(&surface, &error));
    assert(native_shader_surface_view_valid(surface));
    assert(surface.api_kind == NativeGpuApiKind::Vulkan);
    assert(surface.device_generation == context.device_generation);
    assert(surface.runtime_generation == context.runtime_generation);
    assert(surface.executor_generation == config.executor_generation);
    assert(surface.output_generation != 0U);
    assert(surface.frame_id == packets.hot.header.frame_id);
    assert(surface.content_checksum == packets.hot.header.packet_checksum);
    assert(surface.width == packets.hot.header.surface_width);
    assert(surface.height == packets.hot.header.surface_height);
    assert(surface.native_resource != 0U);

    snapshot = executor->snapshot();
    assert(snapshot.executions == 4U);
    assert(snapshot.readbacks == 2U);
    assert(snapshot.last_readback_checksum == packets.reference.checksum);

    GpuShaderPacket corrupted(&packets.hot_resource);
    corrupted.header = packets.hot.header;
    corrupted.header.packet_checksum ^= 1U;
    ShaderReadback rejected;
    assert(!executor->execute(corrupted, packets.atlas, &rejected, &error));
    assert(error.kind == NativeShaderExecutionErrorKind::InvalidInput);

    executor->shutdown();
    window.destroy();
    std::cout << "real Vulkan integer shader execution: commands="
              << packets.hot.header.command_count
              << " fills=" << packets.hot.header.fill_instance_count
              << " glyphs=" << packets.hot.header.glyph_instance_count
              << " checksum=" << packets.reference.checksum
              << " direct_surface=PASS"
              << " lazy_readback=PASS"
              << " PASS\n";
    return 0;
}