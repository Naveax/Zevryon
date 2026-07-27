#pragma once

#include "gpu_device_presentation.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class NativeGpuApiKind : std::uint8_t {
    ReferenceCpu = 0,
    Vulkan,
    Metal,
    Direct3D12
};

enum class NativePresentMode : std::uint8_t {
    Fifo = 0,
    Mailbox,
    Immediate
};

enum NativeDamagePolicyFlags : std::uint32_t {
    kNativeDamageMergeTouching = 1U << 0U,
    kNativeDamageCollapseOnOverflow = 1U << 1U,
    kNativeDamageForceFullRedraw = 1U << 2U
};

struct NativeDamagePolicy final {
    std::uint32_t maximum_rects{0};
    std::uint32_t maximum_commands{0};
    std::uint32_t full_redraw_threshold_permille{0};
    std::uint32_t flags{0};
    std::uint64_t maximum_total_area{0};
    std::uint64_t reserved{0};
};

static_assert(sizeof(NativeDamagePolicy) == 32U);

struct NativeDamageRect final {
    std::int64_t inline_start{0};
    std::int64_t block_start{0};
    std::uint64_t inline_size{0};
    std::uint64_t block_size{0};

    bool operator==(const NativeDamageRect&) const noexcept = default;
};

static_assert(sizeof(NativeDamageRect) == 32U);

enum NativeCommandFootprintFlags : std::uint32_t {
    kNativeCommandFootprintEmpty = 1U << 0U
};

struct NativeCommandFootprint final {
    NativeDamageRect bounds;
    std::uint64_t checksum{0};
    std::uint32_t source_command_index{0};
    GpuFrameCommandKind source_kind{GpuFrameCommandKind::FillRect};
    std::uint32_t flags{0};
    std::uint32_t reserved{0};

    bool operator==(const NativeCommandFootprint&) const noexcept = default;
};

static_assert(sizeof(NativeCommandFootprint) == 56U);

enum class NativeCommandKind : std::uint32_t {
    BeginRenderPass = 0,
    SetScissor,
    FillRect,
    GlyphBatch,
    EndRenderPass
};

enum NativeCommandFlags : std::uint32_t {
    kNativeCommandPartialDamage = 1U << 0U,
    kNativeCommandDuplicatedAcrossDamage = 1U << 1U
};

struct NativeCommandRecord final {
    NativeCommandKind kind{NativeCommandKind::BeginRenderPass};
    std::uint32_t payload_index{0};
    std::uint32_t scissor_index{0};
    std::uint32_t flags{0};

    bool operator==(const NativeCommandRecord&) const noexcept = default;
};

static_assert(sizeof(NativeCommandRecord) == 16U);

class NativeCommandBuffer final {
public:
    explicit NativeCommandBuffer(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    NativeCommandBuffer(const NativeCommandBuffer&) = delete;
    NativeCommandBuffer& operator=(const NativeCommandBuffer&) = delete;
    NativeCommandBuffer(NativeCommandBuffer&&) noexcept = default;
    NativeCommandBuffer& operator=(NativeCommandBuffer&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    GpuSurfaceDescriptor surface;
    std::uint64_t frame_id{0};
    std::uint64_t command_generation{0};
    std::uint64_t source_frame_checksum{0};
    std::uint64_t command_checksum{0};
    std::uint8_t full_redraw{0};
    std::uint8_t reserved[7]{0, 0, 0, 0, 0, 0, 0};
    std::pmr::vector<NativeDamageRect> damage_rects;
    std::pmr::vector<NativeCommandFootprint> footprints;
    std::pmr::vector<NativeCommandRecord> commands;
};

struct NativeCommandBuildRequest final {
    const GpuFrameSubmission* frame{nullptr};
    std::span<const GlyphAtlasDrawInstance> draw_instances;
    const NativeCommandBuffer* previous{nullptr};
    std::span<const NativeDamageRect> invalidations;
    std::uint64_t command_generation{0};
    NativeDamagePolicy policy;
};

enum class NativeCommandBuildErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    FrameTopologyViolation,
    DrawTopologyViolation,
    InvalidDamageRect,
    ArithmeticOverflow,
    DamageLimitExceeded,
    CommandLimitExceeded,
    OutputBudgetExceeded,
    AggregateOverflow
};

struct NativeCommandBuildError final {
    NativeCommandBuildErrorKind kind{NativeCommandBuildErrorKind::None};
    std::size_t command_index{0};
    std::size_t draw_index{0};
    std::size_t damage_index{0};
    std::string message;
};

struct NativeCommandBuildStats final {
    std::uint64_t input_commands{0};
    std::uint64_t input_draw_instances{0};
    std::uint64_t output_footprints{0};
    std::uint64_t changed_commands{0};
    std::uint64_t removed_commands{0};
    std::uint64_t explicit_invalidations{0};
    std::uint64_t merged_damage_rects{0};
    std::uint64_t output_damage_rects{0};
    std::uint64_t output_commands{0};
    std::uint64_t culled_commands{0};
    std::uint64_t duplicated_commands{0};
    std::uint64_t total_damage_area{0};
    std::uint64_t full_surface_area{0};
    std::uint8_t full_redraw{0};
};

const char* native_command_build_error_kind_name(
    NativeCommandBuildErrorKind kind) noexcept;

bool build_native_command_buffer(
    const NativeCommandBuildRequest& request,
    NativeCommandBuffer* output,
    NativeCommandBuildStats* stats,
    NativeCommandBuildError* error) noexcept;

bool native_command_buffer_is_current(
    const GpuFrameSubmission& frame,
    const NativeCommandBuffer& commands) noexcept;

enum class NativeAcquireStatus : std::uint8_t {
    Acquired = 0,
    NotReady,
    OutOfDate,
    DeviceLost
};

enum class NativePresentStatus : std::uint8_t {
    Presented = 0,
    SkippedNoDamage,
    DroppedBackpressure,
    OutOfDate,
    DeviceLost
};

struct NativeSwapchainImageHandle final {
    std::uint64_t device_generation{0};
    std::uint64_t surface_id{0};
    std::uint64_t surface_generation{0};
    std::uint64_t image_generation{0};
    std::uint32_t image_index{0};
    std::uint32_t flags{0};

    bool operator==(const NativeSwapchainImageHandle&) const noexcept = default;
};

static_assert(sizeof(NativeSwapchainImageHandle) == 40U);

struct NativePresentReceipt final {
    NativeSwapchainImageHandle image;
    std::uint64_t frame_id{0};
    std::uint64_t ticket_id{0};
    std::uint64_t signal_fence_value{0};
    std::uint64_t command_checksum{0};
    std::uint32_t command_count{0};
    std::uint32_t damage_rect_count{0};
    NativePresentStatus status{NativePresentStatus::SkippedNoDamage};
    NativePresentMode mode{NativePresentMode::Fifo};
    std::uint8_t reserved[6]{0, 0, 0, 0, 0, 0};

    bool operator==(const NativePresentReceipt&) const noexcept = default;
};

static_assert(sizeof(NativePresentReceipt) == 88U);

struct NativeInFlightFrameRecord final {
    NativePresentReceipt receipt;
    std::uint8_t occupied{0};
    std::uint8_t reserved[7]{0, 0, 0, 0, 0, 0, 0};

    bool operator==(const NativeInFlightFrameRecord&) const noexcept = default;
};

static_assert(sizeof(NativeInFlightFrameRecord) == 96U);

struct NativePresentationConfig final {
    std::uint32_t maximum_frames_in_flight{0};
    std::uint32_t swapchain_image_count{0};
    NativePresentMode present_mode{NativePresentMode::Fifo};
    std::uint8_t drop_when_backpressured{0};
    std::uint16_t reserved{0};
    std::uint64_t device_generation{0};
    std::uint64_t initial_surface_generation{0};
};

static_assert(sizeof(NativePresentationConfig) == 32U);

enum class NativeGpuApiErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    SurfaceConfigurationFailed,
    AcquireFailed,
    EncodeFailed,
    PresentFailed,
    DeviceLost,
    FenceOverflow
};

struct NativeGpuApiError final {
    NativeGpuApiErrorKind kind{NativeGpuApiErrorKind::None};
    std::string message;
};

class NativeGpuCommandApi {
public:
    virtual ~NativeGpuCommandApi() = default;
    virtual NativeGpuApiKind kind() const noexcept = 0;
    virtual bool configure_surface(
        const GpuSurfaceDescriptor& surface,
        std::uint32_t image_count,
        std::uint64_t device_generation,
        NativeGpuApiError* error) noexcept = 0;
    virtual bool acquire_next_image(
        const GpuSurfaceDescriptor& surface,
        NativePresentMode mode,
        std::uint64_t ticket_id,
        NativeSwapchainImageHandle* image,
        NativeAcquireStatus* status,
        NativeGpuApiError* error) noexcept = 0;
    virtual bool encode_submit_present(
        const NativeSwapchainImageHandle& image,
        const NativeCommandBuffer& commands,
        const GpuFrameSubmission& frame,
        std::span<const GlyphAtlasDrawInstance> draw_instances,
        std::uint64_t ticket_id,
        std::uint64_t wait_fence_value,
        std::uint64_t* signal_fence_value,
        std::uint64_t* encoded_checksum,
        NativePresentStatus* status,
        NativeGpuApiError* error) noexcept = 0;
};

class ReferenceNativeGpuCommandApi final : public NativeGpuCommandApi {
public:
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

    void set_next_acquire_status(NativeAcquireStatus status) noexcept;
    void set_next_present_status(NativePresentStatus status) noexcept;

private:
    GpuSurfaceDescriptor surface_;
    std::uint32_t image_count_{0};
    std::uint32_t next_image_index_{0};
    std::uint64_t device_generation_{0};
    std::uint64_t next_image_generation_{1};
    std::uint64_t next_fence_value_{1};
    NativeAcquireStatus next_acquire_status_{NativeAcquireStatus::Acquired};
    NativePresentStatus next_present_status_{NativePresentStatus::Presented};
};

struct NativePresentationSnapshot final {
    core::ResourceSnapshot metadata;
    NativePresentationConfig config;
    GpuSurfaceDescriptor surface;
    std::uint64_t scheduler_generation{0};
    std::uint64_t next_ticket_id{0};
    std::uint64_t completed_fence_value{0};
    std::uint64_t last_submitted_fence_value{0};
    std::uint64_t configured_surfaces{0};
    std::uint64_t acquired_images{0};
    std::uint64_t submitted_frames{0};
    std::uint64_t retired_frames{0};
    std::uint64_t skipped_frames{0};
    std::uint64_t dropped_frames{0};
    std::uint64_t out_of_date_events{0};
    std::uint64_t device_lost_events{0};
    std::uint64_t stale_rejections{0};
    std::size_t in_flight_frame_count{0};
};

class NativePresentationScheduler final {
public:
    NativePresentationScheduler(
        NativePresentationConfig config,
        std::size_t metadata_hard_limit) noexcept;

    NativePresentationScheduler(const NativePresentationScheduler&) = delete;
    NativePresentationScheduler& operator=(const NativePresentationScheduler&) = delete;

    bool clear() noexcept;
    bool retire_completed(
        std::uint64_t completed_fence_value,
        std::string* error) noexcept;
    NativePresentationSnapshot snapshot() const noexcept;

private:
    friend bool submit_native_command_buffer(
        const struct NativePresentRequest&,
        NativeGpuCommandApi*,
        NativePresentationScheduler*,
        NativePresentReceipt*,
        struct NativePresentStats*,
        struct NativePresentError*) noexcept;
    friend bool native_present_receipt_is_current(
        const NativePresentationScheduler&,
        const NativePresentReceipt&) noexcept;

    NativePresentationSnapshot snapshot_locked() const noexcept;

    mutable std::mutex mutex_;
    core::ResourceLedger ledger_;
    core::LedgerMemoryResource metadata_resource_;
    std::pmr::vector<NativeInFlightFrameRecord> frames_;
    NativePresentationConfig config_;
    GpuSurfaceDescriptor surface_;
    std::uint64_t scheduler_generation_{1};
    std::uint64_t next_ticket_id_{1};
    std::uint64_t completed_fence_value_{0};
    std::uint64_t last_submitted_fence_value_{0};
    std::uint64_t configured_surfaces_{0};
    std::uint64_t acquired_images_{0};
    std::uint64_t submitted_frames_{0};
    std::uint64_t retired_frames_{0};
    std::uint64_t skipped_frames_{0};
    std::uint64_t dropped_frames_{0};
    std::uint64_t out_of_date_events_{0};
    std::uint64_t device_lost_events_{0};
    std::uint64_t stale_rejections_{0};
};

struct NativePresentRequest final {
    const NativeCommandBuffer* commands{nullptr};
    const GpuFrameSubmission* frame{nullptr};
    std::span<const GlyphAtlasDrawInstance> draw_instances;
    std::uint64_t wait_fence_value{0};
};

enum class NativePresentErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    StaleCommandBuffer,
    SurfaceConfigurationFailed,
    Backpressure,
    SurfaceNotReady,
    SurfaceOutOfDate,
    DeviceLost,
    BackendFailure,
    FenceRegression,
    MetadataBudgetExceeded,
    AggregateOverflow
};

struct NativePresentError final {
    NativePresentErrorKind kind{NativePresentErrorKind::None};
    NativeGpuApiError backend_error;
    std::string message;
};

struct NativePresentStats final {
    core::ResourceSnapshot metadata_before;
    core::ResourceSnapshot metadata_after;
    std::uint64_t acquired_images{0};
    std::uint64_t submitted_frames{0};
    std::uint64_t skipped_frames{0};
    std::uint64_t dropped_frames{0};
    std::uint64_t command_count{0};
    std::uint64_t damage_rect_count{0};
    std::uint64_t wait_fence_value{0};
    std::uint64_t signal_fence_value{0};
};

const char* native_present_error_kind_name(
    NativePresentErrorKind kind) noexcept;

bool submit_native_command_buffer(
    const NativePresentRequest& request,
    NativeGpuCommandApi* api,
    NativePresentationScheduler* scheduler,
    NativePresentReceipt* receipt,
    NativePresentStats* stats,
    NativePresentError* error) noexcept;

bool native_present_receipt_is_current(
    const NativePresentationScheduler& scheduler,
    const NativePresentReceipt& receipt) noexcept;

} // namespace zevryon::text
