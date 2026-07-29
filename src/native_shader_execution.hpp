#pragma once

#include "native_gpu_sdk_execution.hpp"
#include "shader_draw_packet.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::text {

enum NativeShaderCapabilityFlags : std::uint32_t {
    kNativeShaderComputePipeline = 1U << 0U,
    kNativeShaderPersistentAtlas = 1U << 1U,
    kNativeShaderExactIntegerBlend = 1U << 2U,
    kNativeShaderGpuReadback = 1U << 3U,
    kNativeShaderSameDeviceContext = 1U << 4U,
    kNativeShaderSoftwareDevice = 1U << 5U
};

enum NativeShaderExecutionFlags : std::uint32_t {
    kNativeShaderExecutionReadback = 1U << 0U,
    kNativeShaderExecutionRequireExactReadback = 1U << 1U
};

struct NativeShaderExecutionLimits final {
    std::uint32_t maximum_commands{0U};
    std::uint32_t maximum_scissors{0U};
    std::uint32_t maximum_fill_instances{0U};
    std::uint32_t maximum_glyph_instances{0U};
    std::uint32_t maximum_atlas_pages{0U};
    std::uint32_t maximum_frames_in_flight{0U};
    std::uint32_t maximum_surface_width{0U};
    std::uint32_t maximum_surface_height{0U};
    std::uint64_t maximum_packet_bytes{0U};
    std::uint64_t maximum_atlas_bytes{0U};
    std::uint64_t maximum_output_bytes{0U};
    std::uint64_t maximum_staging_bytes{0U};
};
static_assert(sizeof(NativeShaderExecutionLimits) == 64U);

struct NativeShaderCapabilities final {
    NativeGpuApiKind api_kind{NativeGpuApiKind::ReferenceCpu};
    std::uint8_t reserved0[3]{0U, 0U, 0U};
    std::uint32_t flags{0U};
    std::uint32_t threadgroup_width{8U};
    std::uint32_t threadgroup_height{8U};
    std::uint32_t maximum_atlas_layers{0U};
    std::uint32_t reserved1{0U};
    std::uint64_t shader_source_checksum{0U};
    std::uint64_t reserved2{0U};
};
static_assert(sizeof(NativeShaderCapabilities) == 40U);

struct NativeShaderBindingLayout final {
    std::uint32_t constants_binding{0U};
    std::uint32_t commands_binding{1U};
    std::uint32_t fills_binding{2U};
    std::uint32_t glyphs_binding{3U};
    std::uint32_t scissors_binding{4U};
    std::uint32_t atlas_metadata_binding{5U};
    std::uint32_t atlas_texture_binding{6U};
    std::uint32_t output_binding{7U};
    std::uint32_t threadgroup_width{8U};
    std::uint32_t threadgroup_height{8U};
    std::uint32_t threadgroup_depth{1U};
    std::uint32_t reserved{0U};
};
static_assert(sizeof(NativeShaderBindingLayout) == 48U);

struct NativeShaderConstants final {
    std::uint32_t surface_width{0U};
    std::uint32_t surface_height{0U};
    std::uint32_t command_count{0U};
    std::uint32_t atlas_layer_count{0U};
    std::uint64_t frame_id{0U};
    std::uint64_t packet_checksum{0U};
};
static_assert(sizeof(NativeShaderConstants) == 32U);


struct NativeShaderDrawRecord final {
    std::uint32_t kind{0U};
    std::uint32_t layer{0U};
    std::uint32_t atlas_format{0U};
    std::uint32_t scissor_index{0U};
    std::uint32_t first_instance{0U};
    std::uint32_t instance_count{0U};
    std::uint32_t atlas_page_index{0U};
    std::uint32_t stable_id{0U};
    std::uint32_t reserved[4]{0U, 0U, 0U, 0U};
};
static_assert(sizeof(NativeShaderDrawRecord) == 48U);

struct NativeShaderFillRecord final {
    ShaderRectI destination;
    std::uint32_t packed_color{0U};
    std::uint32_t scissor_index{0U};
    std::uint32_t stable_id{0U};
    std::uint32_t reserved{0U};
};
static_assert(sizeof(NativeShaderFillRecord) == 32U);

struct NativeShaderGlyphRecord final {
    ShaderRectI destination;
    std::uint32_t scissor_index{0U};
    std::uint32_t atlas_page_index{0U};
    std::uint32_t atlas_page_generation{0U};
    std::uint32_t atlas_xy{0U};
    std::uint32_t atlas_wh{0U};
    std::uint32_t packed_color{0U};
    std::uint32_t format{0U};
    std::uint32_t stable_id{0U};
    std::uint32_t reserved[4]{0U, 0U, 0U, 0U};
};
static_assert(sizeof(NativeShaderGlyphRecord) == 64U);

struct NativeShaderScissorRecord final {
    ShaderRectI rect;
};
static_assert(sizeof(NativeShaderScissorRecord) == 16U);

struct NativeShaderAtlasBinding final {
    std::uint32_t page_index{0U};
    std::uint32_t page_generation{0U};
    std::uint32_t texture_layer{0U};
    std::uint16_t width{0U};
    std::uint16_t height{0U};
    std::uint32_t row_bytes{0U};
    std::uint64_t resident_bytes{0U};
    std::uint64_t content_checksum{0U};
};
static_assert(sizeof(NativeShaderAtlasBinding) == 40U);

struct NativeShaderDispatchPlanHeader final {
    NativeGpuApiKind api_kind{NativeGpuApiKind::ReferenceCpu};
    std::uint8_t reserved0[3]{0U, 0U, 0U};
    std::uint32_t flags{0U};
    std::uint32_t dispatch_x{0U};
    std::uint32_t dispatch_y{0U};
    std::uint32_t dispatch_z{1U};
    std::uint32_t atlas_binding_count{0U};
    std::uint64_t command_bytes{0U};
    std::uint64_t fill_bytes{0U};
    std::uint64_t glyph_bytes{0U};
    std::uint64_t scissor_bytes{0U};
    std::uint64_t atlas_bytes{0U};
    std::uint64_t output_bytes{0U};
    std::uint64_t packet_checksum{0U};
    std::uint64_t atlas_checksum{0U};
    std::uint64_t plan_checksum{0U};
};
static_assert(sizeof(NativeShaderDispatchPlanHeader) == 96U);

struct NativeShaderDispatchPlan final {
    explicit NativeShaderDispatchPlan(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    NativeShaderDispatchPlanHeader header;
    NativeShaderBindingLayout bindings;
    NativeShaderConstants constants;
    std::pmr::vector<NativeShaderDrawRecord> commands;
    std::pmr::vector<NativeShaderFillRecord> fills;
    std::pmr::vector<NativeShaderGlyphRecord> glyphs;
    std::pmr::vector<NativeShaderScissorRecord> scissors;
    std::pmr::vector<NativeShaderAtlasBinding> atlas_bindings;

    void clear() noexcept;
};

enum class NativeShaderExecutionErrorKind : std::uint8_t {
    None = 0U,
    InvalidInput,
    UnsupportedBackend,
    StaleGeneration,
    InvalidPacket,
    InvalidAtlasReference,
    ResourceBudgetExceeded,
    AllocationFailed,
    ShaderCompilationFailed,
    PipelineCreationFailed,
    CommandEncodingFailed,
    SubmissionFailed,
    ReadbackFailed,
    ReadbackMismatch,
    DeviceLost
};

struct NativeShaderExecutionError final {
    NativeShaderExecutionErrorKind kind{NativeShaderExecutionErrorKind::None};
    std::int64_t native_code{0};
    std::string message;
};

struct NativeShaderExecutionRequest final {
    const GpuShaderPacket* packet{nullptr};
    const ShaderAtlasResidency* atlas{nullptr};
    std::uint64_t ticket_id{0U};
    std::uint64_t wait_fence_value{0U};
    std::uint64_t expected_readback_checksum{0U};
    std::uint32_t flags{0U};
    std::uint32_t reserved{0U};
};

struct NativeShaderExecutionReceipt final {
    NativeGpuApiKind api_kind{NativeGpuApiKind::ReferenceCpu};
    std::uint8_t reserved0[3]{0U, 0U, 0U};
    std::uint32_t flags{0U};
    std::uint32_t command_count{0U};
    std::uint32_t fill_instance_count{0U};
    std::uint32_t glyph_instance_count{0U};
    std::uint32_t atlas_binding_count{0U};
    std::uint32_t dispatch_x{0U};
    std::uint32_t dispatch_y{0U};
    std::uint64_t frame_id{0U};
    std::uint64_t ticket_id{0U};
    std::uint64_t wait_fence_value{0U};
    std::uint64_t signal_fence_value{0U};
    std::uint64_t packet_checksum{0U};
    std::uint64_t plan_checksum{0U};
    std::uint64_t readback_checksum{0U};
    std::uint64_t output_bytes{0U};
};
static_assert(sizeof(NativeShaderExecutionReceipt) == 96U);

struct NativeShaderExecutionSnapshot final {
    NativeShaderCapabilities capabilities;
    NativeShaderExecutionLimits limits;
    NativeGpuSdkContextHandle context;
    std::uint64_t configurations{0U};
    std::uint64_t executions{0U};
    std::uint64_t readbacks{0U};
    std::uint64_t atlas_uploads{0U};
    std::uint64_t stale_rejections{0U};
    std::uint64_t device_lost_events{0U};
    std::uint64_t current_device_bytes{0U};
    std::uint64_t peak_device_bytes{0U};
    std::uint64_t current_staging_bytes{0U};
    std::uint64_t peak_staging_bytes{0U};
    std::uint64_t last_submitted_fence_value{0U};
    std::uint64_t completed_fence_value{0U};
    std::uint32_t resident_atlas_pages{0U};
    std::uint32_t in_flight_count{0U};
};

const char* native_shader_execution_error_kind_name(
    NativeShaderExecutionErrorKind kind) noexcept;

NativeShaderExecutionLimits default_native_shader_execution_limits() noexcept;
NativeShaderCapabilities default_native_shader_capabilities(
    NativeGpuApiKind kind) noexcept;
NativeShaderBindingLayout native_shader_binding_layout(
    NativeGpuApiKind kind) noexcept;

bool compile_native_shader_dispatch_plan(
    NativeGpuApiKind kind,
    const GpuShaderPacket& packet,
    const ShaderAtlasResidency& atlas,
    const NativeShaderExecutionLimits& limits,
    NativeShaderDispatchPlan* output,
    NativeShaderExecutionError* error) noexcept;

std::uint64_t native_shader_dispatch_plan_checksum(
    const NativeShaderDispatchPlan& plan) noexcept;

class NativeShaderExecutionApi {
public:
    virtual ~NativeShaderExecutionApi() = default;

    virtual NativeGpuApiKind kind() const noexcept = 0;
    virtual NativeShaderCapabilities capabilities() const noexcept = 0;
    virtual bool configure(
        const NativeGpuSdkContextHandle& context,
        const NativeShaderExecutionLimits& limits,
        NativeShaderExecutionError* error) noexcept = 0;
    virtual bool execute(
        const NativeShaderExecutionRequest& request,
        ShaderReadback* readback,
        NativeShaderExecutionReceipt* receipt,
        NativeShaderExecutionError* error) noexcept = 0;
    virtual bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeShaderExecutionError* error) noexcept = 0;
    virtual NativeShaderExecutionSnapshot snapshot() const noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

std::unique_ptr<NativeShaderExecutionApi>
make_reference_native_shader_execution_api() noexcept;
std::unique_ptr<NativeShaderExecutionApi>
make_direct3d12_native_shader_execution_api() noexcept;
std::unique_ptr<NativeShaderExecutionApi>
make_vulkan_native_shader_execution_api() noexcept;
std::unique_ptr<NativeShaderExecutionApi>
make_metal_native_shader_execution_api() noexcept;

std::string_view native_shader_hlsl_source() noexcept;
std::string_view native_shader_glsl_source() noexcept;
std::string_view native_shader_msl_source() noexcept;
std::uint64_t native_shader_source_checksum(NativeGpuApiKind kind) noexcept;

} // namespace zevryon::text
