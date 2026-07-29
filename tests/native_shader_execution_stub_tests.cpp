#include "native_shader_execution.hpp"

#include <cassert>

int main() {
    using namespace zevryon::text;
#if defined(_WIN32)
    assert(native_shader_execution_build_has_backend(
        NativeGpuApiKind::Direct3D12));
#else
    assert(!native_shader_execution_build_has_backend(
        NativeGpuApiKind::Direct3D12));
    assert(make_direct3d12_native_shader_executor() == nullptr);
#endif
    const NativeShaderExecutionLimits limits =
        default_native_shader_execution_limits(NativeGpuApiKind::Direct3D12);
    assert(limits.maximum_commands == 512U);
    assert(limits.maximum_atlas_pages == 64U);
    assert(native_shader_execution_error_kind_name(
        NativeShaderExecutionErrorKind::ChecksumMismatch) != nullptr);
    return 0;
}
