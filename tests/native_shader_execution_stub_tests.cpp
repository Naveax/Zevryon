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
    return 0;
}
