#pragma once

#include "native_gpu_sdk_execution.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace zevryon::text {

enum NativeWindowSwapchainCapabilityFlags : std::uint32_t {
    kNativeWindowSwapchainWindowSurface = 1U << 0U,
    kNativeWindowSwapchainResize = 1U << 1U,
    kNativeWindowSwapchainMailbox = 1U << 2U,
    kNativeWindowSwapchainImmediate = 1U << 3U,
    kNativeWindowSwapchainTearing = 1U << 4U,
    kNativeWindowSwapchainPartialPresent = 1U << 5U,
    kNativeWindowSwapchainOcclusion = 1U << 6U,
    kNativeWindowSwapchainSeparatePresentQueue = 1U << 7U
};

enum NativeWindowSwapchainConfigFlags : std::uint32_t {
    kNativeWindowSwapchainAllowMailbox = 1U << 0U,
    kNativeWindowSwapchainAllowImmediate = 1U << 1U,
    kNativeWindowSwapchainAllowTearing = 1U << 2U,
    kNativeWindowSwapchainAllowPartialPresent = 1U << 3U,
    kNativeWindowSwapchainRequireNativeContext = 1U << 4U
};

struct NativeWindowSwapchainCapabilities final {
    std::uint32_t flags{0};
    std::uint32_t minimum_image_count{2};
    std::uint32_t maximum_image_count{0};
    std::uint32_t maximum_frames_in_flight{0};
    std::uint32_t maximum_damage_rects{0};
    std::uint32_t maximum_width{0};
    std::uint32_t maximum_height{0};
    std::uint32_t reserved{0};
    std::uint64_t maximum_surface_bytes{0};

    bool operator==(const NativeWindowSwapchainCapabilities&) const noexcept = default;
};

static_assert(sizeof(NativeWindowSwapchainCapabilities) == 40U);

struct NativeWindowSwapchainLimits final {
    std::uint32_t maximum_image_count{0};
    std::uint32_t maximum_frames_in_flight{0};
    std::uint32_t maximum_damage_rects{0};
    std::uint32_t maximum_width{0};
    std::uint32_t maximum_height{0};
    std::uint32_t reserved{0};
    std::uint64_t maximum_surface_bytes{0};
    std::uint64_t maximum_in_flight_bytes{0};

    bool operator==(const NativeWindowSwapchainLimits&) const noexcept = default;
};

static_assert(sizeof(NativeWindowSwapchainLimits) == 40U);

struct NativeWindowSwapchainConfig final {
    NativeGpuSdkContextHandle context;
    NativeWindowSurfaceHandle window;
    GpuSurfaceDescriptor surface;
    NativeWindowSwapchainLimits limits;
    std::uint64_t swapchain_generation{0};
    NativePresentMode present_mode{NativePresentMode::Fifo};
    std::uint8_t image_count{0};
    std::uint8_t reserved0[2]{0, 0};
    std::uint32_t flags{0};

    bool operator==(const NativeWindowSwapchainConfig&) const noexcept = default;
};

static_assert(sizeof(NativeWindowSwapchainConfig) == 208U);

enum NativeWindowSwapchainImageFlags : std::uint32_t {
    kNativeWindowSwapchainImageAcquired = 1U << 0U,
    kNativeWindowSwapchainImageSuboptimal = 1U << 1U
};

struct NativeWindowSwapchainImage final {
    NativePlatformSwapchainImage image;
    std::uint64_t swapchain_generation{0};
    std::uint64_t acquire_serial{0};
    std::uint64_t present_serial{0};
    std::uint32_t flags{0};
    std::uint32_t reserved{0};

    bool operator==(const NativeWindowSwapchainImage&) const noexcept = default;
};

static_assert(sizeof(NativeWindowSwapchainImage) == 96U);

enum class NativeWindowAcquireStatus : std::uint8_t {
    Acquired = 0,
    NotReady,
    Suboptimal,
    OutOfDate,
    Occluded,
    DeviceLost
};

enum class NativeWindowPresentStatus : std::uint8_t {
    Presented = 0,
    SkippedNoDamage,
    Suboptimal,
    OutOfDate,
    Occluded,
    DeviceLost
};

enum NativeWindowPresentRequestFlags : std::uint32_t {
    kNativeWindowPresentFullRedraw = 1U << 0U,
    kNativeWindowPresentAllowTearing = 1U << 1U
};

struct NativeWindowPresentRequest final {
    NativeWindowSwapchainImage image;
    std::span<const NativeDamageRect> damage_rects;
    std::uint64_t frame_id{0};
    std::uint64_t ticket_id{0};
    std::uint64_t wait_fence_value{0};
    std::uint64_t command_checksum{0};
    std::uint32_t command_count{0};
    std::uint32_t flags{0};
};

struct NativeWindowPresentReceipt final {
    NativeWindowSwapchainImage image;
    std::uint64_t frame_id{0};
    std::uint64_t ticket_id{0};
    std::uint64_t wait_fence_value{0};
    std::uint64_t signal_fence_value{0};
    std::uint64_t command_checksum{0};
    std::uint32_t command_count{0};
    std::uint32_t damage_rect_count{0};
    NativeWindowPresentStatus status{NativeWindowPresentStatus::SkippedNoDamage};
    std::uint8_t reserved[7]{0, 0, 0, 0, 0, 0, 0};

    bool operator==(const NativeWindowPresentReceipt&) const noexcept = default;
};

static_assert(sizeof(NativeWindowPresentReceipt) == 152U);

enum class NativeWindowSwapchainErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    UnsupportedWindowSystem,
    UnsupportedPresentMode,
    NativeContextUnavailable,
    SurfaceCreationFailed,
    SwapchainCreationFailed,
    ResourceBudgetExceeded,
    AcquireFailed,
    PresentFailed,
    Backpressure,
    OutOfDate,
    Occluded,
    DeviceLost,
    FenceRegression,
    StaleGeneration,
    ArithmeticOverflow
};

struct NativeWindowSwapchainError final {
    NativeWindowSwapchainErrorKind kind{NativeWindowSwapchainErrorKind::None};
    std::int64_t native_code{0};
    std::string message;
};

struct NativeWindowSwapchainSnapshot final {
    NativeWindowSwapchainCapabilities capabilities;
    NativeWindowSwapchainConfig config;
    GpuSurfaceDescriptor pending_surface;
    std::uint64_t configurations{0};
    std::uint64_t resize_requests{0};
    std::uint64_t recreations{0};
    std::uint64_t acquired_images{0};
    std::uint64_t presented_frames{0};
    std::uint64_t skipped_frames{0};
    std::uint64_t out_of_date_events{0};
    std::uint64_t occlusion_events{0};
    std::uint64_t device_lost_events{0};
    std::uint64_t stale_rejections{0};
    std::uint64_t last_submitted_fence_value{0};
    std::uint64_t completed_fence_value{0};
    std::uint64_t current_surface_bytes{0};
    std::uint64_t peak_surface_bytes{0};
    std::uint64_t current_in_flight_bytes{0};
    std::uint64_t peak_in_flight_bytes{0};
    std::uint32_t configured_image_count{0};
    std::uint32_t acquired_image_count{0};
    std::uint32_t in_flight_frame_count{0};
    std::uint8_t configured{0};
    std::uint8_t out_of_date{0};
    std::uint8_t occluded{0};
    std::uint8_t device_lost{0};
};

const char* native_window_swapchain_error_kind_name(
    NativeWindowSwapchainErrorKind kind) noexcept;

class NativeWindowSwapchainApi {
public:
    virtual ~NativeWindowSwapchainApi() = default;

    virtual NativeWindowSwapchainCapabilities capabilities() const noexcept = 0;
    virtual bool configure(
        const NativeWindowSwapchainConfig& config,
        NativeWindowSwapchainError* error) noexcept = 0;
    virtual bool request_resize(
        const GpuSurfaceDescriptor& surface,
        NativeWindowSwapchainError* error) noexcept = 0;
    virtual bool recreate(
        const NativeWindowSwapchainConfig& config,
        NativeWindowSwapchainError* error) noexcept = 0;
    virtual bool acquire(
        std::uint64_t ticket_id,
        NativeWindowSwapchainImage* image,
        NativeWindowAcquireStatus* status,
        NativeWindowSwapchainError* error) noexcept = 0;
    virtual bool present(
        const NativeWindowPresentRequest& request,
        NativeWindowPresentReceipt* receipt,
        NativeWindowSwapchainError* error) noexcept = 0;
    virtual bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeWindowSwapchainError* error) noexcept = 0;
    virtual NativeWindowSwapchainSnapshot snapshot() const noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

class ReferenceNativeWindowSwapchainApi final : public NativeWindowSwapchainApi {
public:
    explicit ReferenceNativeWindowSwapchainApi(
        NativeWindowSwapchainCapabilities capabilities) noexcept;

    NativeWindowSwapchainCapabilities capabilities() const noexcept override;
    bool configure(
        const NativeWindowSwapchainConfig& config,
        NativeWindowSwapchainError* error) noexcept override;
    bool request_resize(
        const GpuSurfaceDescriptor& surface,
        NativeWindowSwapchainError* error) noexcept override;
    bool recreate(
        const NativeWindowSwapchainConfig& config,
        NativeWindowSwapchainError* error) noexcept override;
    bool acquire(
        std::uint64_t ticket_id,
        NativeWindowSwapchainImage* image,
        NativeWindowAcquireStatus* status,
        NativeWindowSwapchainError* error) noexcept override;
    bool present(
        const NativeWindowPresentRequest& request,
        NativeWindowPresentReceipt* receipt,
        NativeWindowSwapchainError* error) noexcept override;
    bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeWindowSwapchainError* error) noexcept override;
    NativeWindowSwapchainSnapshot snapshot() const noexcept override;
    void shutdown() noexcept override;

    void set_next_acquire_status(NativeWindowAcquireStatus status) noexcept;
    void set_next_present_status(NativeWindowPresentStatus status) noexcept;
    void set_occluded(bool occluded) noexcept;
    void set_device_lost(bool lost) noexcept;

private:
    struct ImageSlot final {
        NativeWindowSwapchainImage image;
        std::uint64_t fence_value{0};
        std::uint8_t acquired{0};
        std::uint8_t in_flight{0};
        std::uint8_t reserved[6]{0, 0, 0, 0, 0, 0};
    };

    bool configure_locked(
        const NativeWindowSwapchainConfig& config,
        bool recreation,
        NativeWindowSwapchainError* error) noexcept;
    void reset_images_locked() noexcept;

    mutable std::mutex mutex_;
    NativeWindowSwapchainSnapshot snapshot_;
    std::array<ImageSlot, 16U> images_{};
    std::uint32_t next_image_index_{0};
    std::uint64_t next_image_generation_{1};
    std::uint64_t next_resource_id_{1};
    std::uint64_t next_acquire_serial_{1};
    std::uint64_t next_present_serial_{1};
    std::uint64_t next_fence_value_{1};
    NativeWindowAcquireStatus next_acquire_status_{NativeWindowAcquireStatus::Acquired};
    NativeWindowPresentStatus next_present_status_{NativeWindowPresentStatus::Presented};
};

std::unique_ptr<NativeWindowSwapchainApi>
make_direct3d12_native_window_swapchain_api() noexcept;

bool native_window_swapchain_build_has_backend(
    NativeGpuApiKind kind,
    NativeWindowSystem system) noexcept;

NativeWindowSwapchainCapabilities default_native_window_swapchain_capabilities(
    NativeGpuApiKind kind,
    NativeWindowSystem system) noexcept;

NativeWindowSwapchainLimits default_native_window_swapchain_limits(
    NativeGpuApiKind kind,
    NativeWindowSystem system) noexcept;

} // namespace zevryon::text
