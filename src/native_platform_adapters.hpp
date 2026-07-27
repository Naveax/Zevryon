#pragma once

#include "native_damage_presentation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum NativePlatformCapabilityFlags : std::uint32_t {
    kNativePlatformTimelineFence = 1U << 0U,
    kNativePlatformPartialPresent = 1U << 1U,
    kNativePlatformMailboxPresent = 1U << 2U,
    kNativePlatformImmediatePresent = 1U << 3U,
    kNativePlatformTearing = 1U << 4U,
    kNativePlatformExplicitBarriers = 1U << 5U,
    kNativePlatformUnifiedMemory = 1U << 6U
};

struct NativePlatformCapabilities final {
    std::uint32_t flags{0};
    std::uint32_t maximum_commands{0};
    std::uint32_t maximum_barriers{0};
    std::uint32_t maximum_descriptors{0};
    std::uint32_t maximum_swapchain_images{0};
    std::uint32_t maximum_frames_in_flight{0};
    std::uint64_t maximum_staging_bytes{0};

    bool operator==(const NativePlatformCapabilities&) const noexcept = default;
};

static_assert(sizeof(NativePlatformCapabilities) == 32U);

enum NativePlatformAdapterFlags : std::uint32_t {
    kNativePlatformRequireTimelineFence = 1U << 0U,
    kNativePlatformAllowMailbox = 1U << 1U,
    kNativePlatformAllowImmediate = 1U << 2U,
    kNativePlatformAllowTearing = 1U << 3U
};

struct NativePlatformAdapterConfig final {
    NativeGpuApiKind api_kind{NativeGpuApiKind::ReferenceCpu};
    std::uint8_t reserved0[3]{0, 0, 0};
    std::uint32_t maximum_commands{0};
    std::uint32_t maximum_barriers{0};
    std::uint32_t maximum_descriptors{0};
    std::uint32_t maximum_swapchain_images{0};
    std::uint32_t maximum_frames_in_flight{0};
    std::uint32_t flags{0};
    std::uint64_t device_generation{0};
    std::uint64_t driver_generation{0};

    bool operator==(const NativePlatformAdapterConfig&) const noexcept = default;
};

static_assert(sizeof(NativePlatformAdapterConfig) == 48U);

enum class NativePlatformResourceState : std::uint32_t {
    Undefined = 0,
    CopyDestination,
    ShaderRead,
    RenderTarget,
    Present
};

enum class NativePlatformCommandKind : std::uint32_t {
    BeginCommandBuffer = 0,
    Transition,
    BeginRenderPass,
    SetScissor,
    FillRect,
    BindGlyphTexture,
    DrawGlyphBatch,
    EndRenderPass,
    EndCommandBuffer,
    Submit,
    Present
};

enum NativePlatformCommandFlags : std::uint32_t {
    kNativePlatformCommandPartialDamage = 1U << 0U,
    kNativePlatformCommandDuplicatedAcrossDamage = 1U << 1U,
    kNativePlatformCommandWaitsForUpload = 1U << 2U,
    kNativePlatformCommandAllowTearing = 1U << 3U
};

struct NativePlatformCommandRecord final {
    NativePlatformCommandKind kind{NativePlatformCommandKind::BeginCommandBuffer};
    std::uint32_t source_index{0};
    std::uint32_t auxiliary_index{0};
    std::uint32_t flags{0};
    std::uint64_t value0{0};
    std::uint64_t value1{0};

    bool operator==(const NativePlatformCommandRecord&) const noexcept = default;
};

static_assert(sizeof(NativePlatformCommandRecord) == 32U);

struct NativePlatformBarrierRecord final {
    std::uint64_t resource_id{0};
    std::uint64_t resource_generation{0};
    NativePlatformResourceState before{NativePlatformResourceState::Undefined};
    NativePlatformResourceState after{NativePlatformResourceState::Undefined};
    std::uint32_t source_command_index{0};
    std::uint32_t flags{0};

    bool operator==(const NativePlatformBarrierRecord&) const noexcept = default;
};

static_assert(sizeof(NativePlatformBarrierRecord) == 32U);

struct NativePlatformDescriptorBinding final {
    std::uint64_t atlas_generation_id{0};
    std::uint64_t page_generation{0};
    std::uint32_t page_index{0};
    std::uint32_t descriptor_slot{0};
    GlyphRasterFormat format{GlyphRasterFormat::Alpha8};
    std::uint8_t reserved0{0};
    std::uint16_t reserved1{0};
    std::uint32_t flags{0};

    bool operator==(const NativePlatformDescriptorBinding&) const noexcept = default;
};

static_assert(sizeof(NativePlatformDescriptorBinding) == 32U);

struct NativePlatformSwapchainImage final {
    NativeSwapchainImageHandle image;
    std::uint64_t driver_generation{0};
    std::uint64_t native_resource_id{0};
    NativePlatformResourceState state{NativePlatformResourceState::Present};
    std::uint32_t reserved{0};

    bool operator==(const NativePlatformSwapchainImage&) const noexcept = default;
};

static_assert(sizeof(NativePlatformSwapchainImage) == 64U);

class NativePlatformSubmission final {
public:
    explicit NativePlatformSubmission(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    NativePlatformSubmission(const NativePlatformSubmission&) = delete;
    NativePlatformSubmission& operator=(const NativePlatformSubmission&) = delete;
    NativePlatformSubmission(NativePlatformSubmission&&) noexcept = default;
    NativePlatformSubmission& operator=(NativePlatformSubmission&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    NativeGpuApiKind api_kind{NativeGpuApiKind::ReferenceCpu};
    GpuSurfaceDescriptor surface;
    NativePlatformSwapchainImage image;
    std::uint64_t frame_id{0};
    std::uint64_t ticket_id{0};
    std::uint64_t wait_fence_value{0};
    std::uint64_t command_generation{0};
    std::uint64_t source_command_checksum{0};
    std::uint64_t encoded_checksum{0};
    std::pmr::vector<NativePlatformCommandRecord> commands;
    std::pmr::vector<NativePlatformBarrierRecord> barriers;
    std::pmr::vector<NativePlatformDescriptorBinding> descriptors;
};

struct NativePlatformCompileRequest final {
    const NativeCommandBuffer* commands{nullptr};
    const GpuFrameSubmission* frame{nullptr};
    std::span<const GlyphAtlasDrawInstance> draw_instances;
    NativePlatformSwapchainImage image;
    std::uint64_t ticket_id{0};
    std::uint64_t wait_fence_value{0};
    NativePlatformAdapterConfig config;
};

enum class NativePlatformCompileErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    UnsupportedCapability,
    StaleCommandBuffer,
    StaleSwapchainImage,
    CommandTopologyViolation,
    DrawTopologyViolation,
    UploadFenceNotReady,
    CommandCapacityExceeded,
    BarrierCapacityExceeded,
    DescriptorCapacityExceeded,
    OutputBudgetExceeded,
    ArithmeticOverflow,
    AggregateOverflow
};

struct NativePlatformCompileError final {
    NativePlatformCompileErrorKind kind{NativePlatformCompileErrorKind::None};
    std::size_t command_index{0};
    std::size_t draw_index{0};
    std::uint32_t page_index{0};
    std::string message;
};

struct NativePlatformCompileStats final {
    std::uint64_t input_native_commands{0};
    std::uint64_t input_damage_rects{0};
    std::uint64_t output_commands{0};
    std::uint64_t output_barriers{0};
    std::uint64_t output_descriptors{0};
    std::uint64_t fill_commands{0};
    std::uint64_t glyph_draw_commands{0};
    std::uint64_t scissor_commands{0};
    std::uint64_t duplicated_commands{0};
    std::uint64_t waited_pages{0};
    std::uint64_t maximum_instances_per_draw{0};
};

const char* native_platform_compile_error_kind_name(
    NativePlatformCompileErrorKind kind) noexcept;

bool compile_native_platform_submission(
    const NativePlatformCompileRequest& request,
    NativePlatformSubmission* output,
    NativePlatformCompileStats* stats,
    NativePlatformCompileError* error) noexcept;

class NativePlatformDriver {
public:
    virtual ~NativePlatformDriver() = default;
    virtual NativeGpuApiKind kind() const noexcept = 0;
    virtual NativePlatformCapabilities capabilities() const noexcept = 0;
    virtual bool configure_swapchain(
        const GpuSurfaceDescriptor& surface,
        std::uint32_t image_count,
        const NativePlatformAdapterConfig& config,
        NativeGpuApiError* error) noexcept = 0;
    virtual bool acquire_image(
        const GpuSurfaceDescriptor& surface,
        NativePresentMode mode,
        std::uint64_t ticket_id,
        NativePlatformSwapchainImage* image,
        NativeAcquireStatus* status,
        NativeGpuApiError* error) noexcept = 0;
    virtual bool submit_and_present(
        const NativePlatformSubmission& submission,
        std::uint64_t* signal_fence_value,
        std::uint64_t* encoded_checksum,
        NativePresentStatus* status,
        NativeGpuApiError* error) noexcept = 0;
};

class ReferenceNativePlatformDriver final : public NativePlatformDriver {
public:
    explicit ReferenceNativePlatformDriver(
        NativeGpuApiKind kind,
        NativePlatformCapabilities capabilities = {}) noexcept;

    NativeGpuApiKind kind() const noexcept override;
    NativePlatformCapabilities capabilities() const noexcept override;
    bool configure_swapchain(
        const GpuSurfaceDescriptor& surface,
        std::uint32_t image_count,
        const NativePlatformAdapterConfig& config,
        NativeGpuApiError* error) noexcept override;
    bool acquire_image(
        const GpuSurfaceDescriptor& surface,
        NativePresentMode mode,
        std::uint64_t ticket_id,
        NativePlatformSwapchainImage* image,
        NativeAcquireStatus* status,
        NativeGpuApiError* error) noexcept override;
    bool submit_and_present(
        const NativePlatformSubmission& submission,
        std::uint64_t* signal_fence_value,
        std::uint64_t* encoded_checksum,
        NativePresentStatus* status,
        NativeGpuApiError* error) noexcept override;

    void set_next_acquire_status(NativeAcquireStatus status) noexcept;
    void set_next_present_status(NativePresentStatus status) noexcept;

private:
    NativeGpuApiKind kind_{NativeGpuApiKind::ReferenceCpu};
    NativePlatformCapabilities capabilities_;
    GpuSurfaceDescriptor surface_;
    std::uint32_t image_count_{0};
    std::uint32_t next_image_index_{0};
    std::uint64_t device_generation_{0};
    std::uint64_t driver_generation_{0};
    std::uint64_t next_image_generation_{1};
    std::uint64_t next_native_resource_id_{1};
    std::uint64_t next_fence_value_{1};
    NativeAcquireStatus next_acquire_status_{NativeAcquireStatus::Acquired};
    NativePresentStatus next_present_status_{NativePresentStatus::Presented};
};

class NativePlatformGpuCommandApi : public NativeGpuCommandApi {
public:
    NativePlatformGpuCommandApi(
        NativePlatformDriver* driver,
        NativePlatformAdapterConfig config) noexcept;

    NativeGpuApiKind kind() const noexcept override;
    bool configure_surface(
        const GpuSurfaceDescriptor& surface,
        std::uint32_t image_count,
        std::uint64_t device_generation,
        NativeGpuApiError* error) noexcept override;
    bool acquire_next_image(
        const GpuSurfaceDescriptor& surface,
        NativePresentMode mode,
        std::uint64_t ticket_id,
        NativeSwapchainImageHandle* image,
        NativeAcquireStatus* status,
        NativeGpuApiError* error) noexcept override;
    bool encode_submit_present(
        const NativeSwapchainImageHandle& image,
        const NativeCommandBuffer& commands,
        const GpuFrameSubmission& frame,
        std::span<const GlyphAtlasDrawInstance> draw_instances,
        std::uint64_t ticket_id,
        std::uint64_t wait_fence_value,
        std::uint64_t* signal_fence_value,
        std::uint64_t* encoded_checksum,
        NativePresentStatus* status,
        NativeGpuApiError* error) noexcept override;

    NativePlatformAdapterConfig config() const noexcept;
    NativePlatformCapabilities capabilities() const noexcept;

private:
    mutable std::mutex mutex_;
    NativePlatformDriver* driver_{nullptr};
    NativePlatformAdapterConfig config_;
    NativePlatformCapabilities capabilities_;
    GpuSurfaceDescriptor surface_;
    std::uint32_t image_count_{0};
    std::array<NativePlatformSwapchainImage, 16U> acquired_images_{};
    std::uint32_t acquired_image_count_{0};
    bool configured_{false};
};

class VulkanNativeGpuCommandApi final : public NativePlatformGpuCommandApi {
public:
    VulkanNativeGpuCommandApi(
        NativePlatformDriver* driver,
        NativePlatformAdapterConfig config) noexcept;
};

class MetalNativeGpuCommandApi final : public NativePlatformGpuCommandApi {
public:
    MetalNativeGpuCommandApi(
        NativePlatformDriver* driver,
        NativePlatformAdapterConfig config) noexcept;
};

class Direct3D12NativeGpuCommandApi final : public NativePlatformGpuCommandApi {
public:
    Direct3D12NativeGpuCommandApi(
        NativePlatformDriver* driver,
        NativePlatformAdapterConfig config) noexcept;
};

NativePlatformCapabilities default_native_platform_capabilities(
    NativeGpuApiKind kind) noexcept;

} // namespace zevryon::text
