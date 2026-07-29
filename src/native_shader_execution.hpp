#pragma once

#include "native_gpu_sdk_execution.hpp"
#include "native_shader_surface.hpp"
#include "shader_draw_packet.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace zevryon::text {

enum NativeShaderExecutionCapabilityFlags : std::uint32_t {
    kNativeShaderExecutionIntegerComposition = 1U << 0U,
    kNativeShaderExecutionPersistentAtlas = 1U << 1U,
    kNativeShaderExecutionGpuReadback = 1U << 2U,
    kNativeShaderExecutionRetainedContext = 1U << 3U,
    kNativeShaderExecutionDirectSurfaceExport = 1U << 4U
};

struct NativeShaderExecutionLimits final {
    std::uint32_t maximum_commands{0U};
    std::uint32_t maximum_fill_instances{0U};
    std::uint32_t maximum_glyph_instances{0U};
    std::uint32_t maximum_atlas_pages{0U};
    std::uint32_t maximum_surface_width{0U};
    std::uint32_t maximum_surface_height{0U};
    std::uint64_t maximum_packet_bytes{0U};
    std::uint64_t maximum_atlas_bytes{0U};
    std::uint64_t maximum_readback_bytes{0U};
};
static_assert(sizeof(NativeShaderExecutionLimits) == 48U);

struct NativeShaderExecutionConfig final {
    NativeGpuSdkContextHandle context;
    NativeShaderExecutionLimits limits;
    std::uint64_t executor_generation{0U};
    std::uint64_t reserved{0U};
};
static_assert(sizeof(NativeShaderExecutionConfig) == 136U);

enum class NativeShaderExecutionErrorKind : std::uint8_t {
    None = 0U,
    InvalidInput,
    UnsupportedBackend,
    NativeContextUnavailable,
    ShaderCompilationFailed,
    PipelineCreationFailed,
    ResourceAllocationFailed,
    ResourceBudgetExceeded,
    AtlasUploadFailed,
    CommandEncodingFailed,
    SubmissionFailed,
    ReadbackFailed,
    FenceTimeout,
    DeviceLost,
    StaleGeneration,
    ChecksumMismatch,
    AllocationFailed
};

struct NativeShaderExecutionError final {
    NativeShaderExecutionErrorKind kind{NativeShaderExecutionErrorKind::None};
    std::int64_t native_code{0};
    std::string message;
};

struct NativeShaderExecutionSnapshot final {
    NativeGpuApiKind api_kind{NativeGpuApiKind::ReferenceCpu};
    std::uint8_t configured{0U};
    std::uint8_t reserved0[3]{0U, 0U, 0U};
    std::uint32_t capability_flags{0U};
    std::uint64_t device_generation{0U};
    std::uint64_t runtime_generation{0U};
    std::uint64_t executor_generation{0U};
    std::uint64_t executions{0U};
    std::uint64_t atlas_upload_batches{0U};
    std::uint64_t atlas_reuses{0U};
    std::uint64_t readbacks{0U};
    std::uint64_t rejected_packets{0U};
    std::uint64_t last_packet_checksum{0U};
    std::uint64_t last_readback_checksum{0U};
    std::uint64_t persistent_atlas_bytes{0U};
    std::uint64_t output_surface_bytes{0U};
    std::uint64_t peak_transient_bytes{0U};
};
static_assert(sizeof(NativeShaderExecutionSnapshot) == 120U);

const char* native_shader_execution_error_kind_name(
    NativeShaderExecutionErrorKind kind) noexcept;

class NativeShaderExecutor {
public:
    virtual ~NativeShaderExecutor() = default;

    virtual NativeGpuApiKind kind() const noexcept = 0;
    virtual bool configure(
        const NativeShaderExecutionConfig& config,
        NativeShaderExecutionError* error) noexcept = 0;
    virtual bool execute(
        const GpuShaderPacket& packet,
        const ShaderAtlasResidency& atlas,
        ShaderReadback* readback,
        NativeShaderExecutionError* error) noexcept = 0;
    virtual bool export_surface(
        NativeShaderSurfaceView* surface,
        NativeShaderExecutionError* error) noexcept;
    virtual NativeShaderExecutionSnapshot snapshot() const noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

NativeShaderExecutionLimits default_native_shader_execution_limits(
    NativeGpuApiKind kind) noexcept;

std::unique_ptr<NativeShaderExecutor>
make_direct3d12_native_shader_executor() noexcept;

std::unique_ptr<NativeShaderExecutor>
make_vulkan_native_shader_executor() noexcept;

std::unique_ptr<NativeShaderExecutor>
make_metal_native_shader_executor() noexcept;

bool native_shader_execution_build_has_backend(
    NativeGpuApiKind kind) noexcept;

} // namespace zevryon::text
