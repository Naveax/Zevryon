#include "native_shader_execution.hpp"

namespace zevryon::text {

const char* native_shader_execution_error_kind_name(
    NativeShaderExecutionErrorKind kind) noexcept {
    switch (kind) {
        case NativeShaderExecutionErrorKind::None: return "none";
        case NativeShaderExecutionErrorKind::InvalidInput: return "invalid-input";
        case NativeShaderExecutionErrorKind::UnsupportedBackend: return "unsupported-backend";
        case NativeShaderExecutionErrorKind::NativeContextUnavailable: return "native-context-unavailable";
        case NativeShaderExecutionErrorKind::ShaderCompilationFailed: return "shader-compilation-failed";
        case NativeShaderExecutionErrorKind::PipelineCreationFailed: return "pipeline-creation-failed";
        case NativeShaderExecutionErrorKind::ResourceAllocationFailed: return "resource-allocation-failed";
        case NativeShaderExecutionErrorKind::ResourceBudgetExceeded: return "resource-budget-exceeded";
        case NativeShaderExecutionErrorKind::AtlasUploadFailed: return "atlas-upload-failed";
        case NativeShaderExecutionErrorKind::CommandEncodingFailed: return "command-encoding-failed";
        case NativeShaderExecutionErrorKind::SubmissionFailed: return "submission-failed";
        case NativeShaderExecutionErrorKind::ReadbackFailed: return "readback-failed";
        case NativeShaderExecutionErrorKind::FenceTimeout: return "fence-timeout";
        case NativeShaderExecutionErrorKind::DeviceLost: return "device-lost";
        case NativeShaderExecutionErrorKind::StaleGeneration: return "stale-generation";
        case NativeShaderExecutionErrorKind::ChecksumMismatch: return "checksum-mismatch";
        case NativeShaderExecutionErrorKind::AllocationFailed: return "allocation-failed";
    }
    return "unknown";
}

NativeShaderExecutionLimits default_native_shader_execution_limits(
    NativeGpuApiKind kind) noexcept {
    NativeShaderExecutionLimits limits{};
    if (kind == NativeGpuApiKind::Direct3D12 ||
        kind == NativeGpuApiKind::Vulkan ||
        kind == NativeGpuApiKind::Metal) {
        limits.maximum_commands = 512U;
        limits.maximum_fill_instances = 4096U;
        limits.maximum_glyph_instances = 65'536U;
        limits.maximum_atlas_pages = 64U;
        limits.maximum_surface_width = 8192U;
        limits.maximum_surface_height = 8192U;
        limits.maximum_packet_bytes = 16U * 1024U * 1024U;
        limits.maximum_atlas_bytes = 64U * 1024U * 1024U;
        limits.maximum_readback_bytes = 256U * 1024U * 1024U;
    }
    return limits;
}

#if !defined(ZEVRYON_HAS_D3D12_SHADER_EXECUTION)
std::unique_ptr<NativeShaderExecutor>
make_direct3d12_native_shader_executor() noexcept {
    return nullptr;
}
#endif

#if !defined(ZEVRYON_HAS_VULKAN_SHADER_EXECUTION)
std::unique_ptr<NativeShaderExecutor>
make_vulkan_native_shader_executor() noexcept {
    return nullptr;
}
#endif

#if !defined(ZEVRYON_HAS_METAL_SHADER_EXECUTION)
std::unique_ptr<NativeShaderExecutor>
make_metal_native_shader_executor() noexcept {
    return nullptr;
}
#endif

bool native_shader_execution_build_has_backend(
    NativeGpuApiKind kind) noexcept {
    bool available = false;
#if defined(ZEVRYON_HAS_D3D12_SHADER_EXECUTION)
    available = available || kind == NativeGpuApiKind::Direct3D12;
#endif
#if defined(ZEVRYON_HAS_VULKAN_SHADER_EXECUTION)
    available = available || kind == NativeGpuApiKind::Vulkan;
#endif
#if defined(ZEVRYON_HAS_METAL_SHADER_EXECUTION)
    available = available || kind == NativeGpuApiKind::Metal;
#endif
    (void)kind;
    return available;
}

} // namespace zevryon::text
