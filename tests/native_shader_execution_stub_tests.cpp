#include "native_shader_execution.hpp"

int main() {
    using namespace zevryon::text;
#if defined(_WIN32)
    if (!native_shader_execution_build_has_backend(
            NativeGpuApiKind::Direct3D12)) {
        return 1;
    }
#else
    if (native_shader_execution_build_has_backend(
            NativeGpuApiKind::Direct3D12) ||
        make_direct3d12_native_shader_executor() != nullptr) {
        return 2;
    }
#endif
    const NativeShaderExecutionLimits limits =
        default_native_shader_execution_limits(NativeGpuApiKind::Direct3D12);
    if (limits.maximum_commands != 512U ||
        limits.maximum_atlas_pages != 64U) {
        return 3;
    }
    if (native_shader_execution_error_kind_name(
            NativeShaderExecutionErrorKind::ChecksumMismatch) == nullptr) {
        return 4;
    }

    const NativeShaderExecutionLimits metal_limits =
        default_native_shader_execution_limits(NativeGpuApiKind::Metal);
    if (native_shader_execution_build_has_backend(NativeGpuApiKind::Metal) ||
        make_metal_native_shader_executor() != nullptr ||
        metal_limits.maximum_commands != 0U ||
        metal_limits.maximum_fill_instances != 0U ||
        metal_limits.maximum_glyph_instances != 0U ||
        metal_limits.maximum_atlas_pages != 0U ||
        metal_limits.maximum_surface_width != 0U ||
        metal_limits.maximum_surface_height != 0U ||
        metal_limits.maximum_packet_bytes != 0U ||
        metal_limits.maximum_atlas_bytes != 0U ||
        metal_limits.maximum_readback_bytes != 0U) {
        return 5;
    }
    return 0;
}
