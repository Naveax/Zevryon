#pragma once

#include "native_platform_adapters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace zevryon::text {

enum class NativeGpuSdkAvailability : std::uint8_t {
    Unavailable = 0,
    CompileOnly,
    RuntimeReady
};

enum class NativeWindowSystem : std::uint8_t {
    Headless = 0,
    Win32,
    CocoaLayer,
    Xcb,
    Wayland
};

enum NativeGpuSdkCapabilityFlags : std::uint32_t {
    kNativeGpuSdkRealDevice = 1U << 0U,
    kNativeGpuSdkOffscreenSurface = 1U << 1U,
    kNativeGpuSdkWindowSurface = 1U << 2U,
    kNativeGpuSdkTimelineFence = 1U << 3U,
    kNativeGpuSdkDedicatedTransferQueue = 1U << 4U,
    kNativeGpuSdkSoftwareDevice = 1U << 5U,
    kNativeGpuSdkUnifiedMemory = 1U << 6U,
    kNativeGpuSdkDebugValidation = 1U << 7U
};

struct NativeWindowSurfaceHandle final {
    std::uint64_t generation{0};
    std::uint64_t display_or_instance{0};
    std::uint64_t window_or_layer{0};
    std::uint64_t auxiliary{0};
    NativeWindowSystem system{NativeWindowSystem::Headless};
    std::uint8_t reserved0[3]{0, 0, 0};
    std::uint32_t flags{0};
    std::uint64_t reserved1{0};

    bool operator==(const NativeWindowSurfaceHandle&) const noexcept = default;
};

static_assert(sizeof(NativeWindowSurfaceHandle) == 48U);

struct NativeGpuSdkLimits final {
    std::uint32_t maximum_swapchain_images{0};
    std::uint32_t maximum_frames_in_flight{0};
    std::uint32_t maximum_command_allocators{0};
    std::uint32_t maximum_descriptors{0};
    std::uint32_t maximum_texture_resources{0};
    std::uint32_t reserved{0};
    std::uint64_t maximum_staging_bytes{0};
    std::uint64_t maximum_device_local_bytes{0};
    std::uint64_t maximum_submission_commands{0};

    bool operator==(const NativeGpuSdkLimits&) const noexcept = default;
};

static_assert(sizeof(NativeGpuSdkLimits) == 48U);

struct NativeGpuSdkConfig final {
    NativeGpuApiKind api_kind{NativeGpuApiKind::ReferenceCpu};
    std::uint8_t allow_software_device{1};
    std::uint8_t require_real_device{0};
    std::uint8_t enable_validation{0};
    std::uint32_t reserved0{0};
    std::uint64_t device_generation{0};
    std::uint64_t runtime_generation{0};
    NativeGpuSdkLimits limits;
    NativeWindowSurfaceHandle window;
};

static_assert(sizeof(NativeGpuSdkConfig) == 120U);

struct NativeGpuSdkProbe final {
    NativeGpuApiKind api_kind{NativeGpuApiKind::ReferenceCpu};
    NativeGpuSdkAvailability availability{NativeGpuSdkAvailability::Unavailable};
    std::uint16_t api_major{0};
    std::uint16_t api_minor{0};
    std::uint16_t api_patch{0};
    std::uint32_t flags{0};
    std::uint32_t vendor_id{0};
    std::uint32_t device_id{0};
    std::uint32_t queue_family_index{0};
    std::uint32_t reserved0{0};
    std::uint64_t dedicated_video_memory_bytes{0};
    std::uint64_t shared_system_memory_bytes{0};
    std::uint64_t runtime_generation{0};
    std::uint64_t checksum{0};

    bool operator==(const NativeGpuSdkProbe&) const noexcept = default;
};

static_assert(sizeof(NativeGpuSdkProbe) == 64U);

enum class NativeGpuSdkErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    UnsupportedBackend,
    RuntimeUnavailable,
    DeviceCreationFailed,
    QueueCreationFailed,
    SurfaceConfigurationFailed,
    ResourceAllocationFailed,
    ResourceBudgetExceeded,
    AcquireFailed,
    CommandEncodingFailed,
    SubmissionFailed,
    PresentFailed,
    FenceRegression,
    DeviceLost,
    StaleGeneration,
    AggregateOverflow
};

struct NativeGpuSdkError final {
    NativeGpuSdkErrorKind kind{NativeGpuSdkErrorKind::None};
    std::int64_t native_code{0};
    std::string message;
};

struct NativeGpuSdkSubmissionReceipt final {
    NativeGpuApiKind api_kind{NativeGpuApiKind::ReferenceCpu};
    NativePresentStatus status{NativePresentStatus::SkippedNoDamage};
    std::uint16_t reserved0{0};
    std::uint32_t command_count{0};
    std::uint32_t barrier_count{0};
    std::uint32_t descriptor_count{0};
    std::uint32_t image_index{0};
    std::uint64_t device_generation{0};
    std::uint64_t runtime_generation{0};
    std::uint64_t surface_generation{0};
    std::uint64_t frame_id{0};
    std::uint64_t ticket_id{0};
    std::uint64_t wait_fence_value{0};
    std::uint64_t signal_fence_value{0};
    std::uint64_t encoded_checksum{0};

    bool operator==(const NativeGpuSdkSubmissionReceipt&) const noexcept = default;
};

static_assert(sizeof(NativeGpuSdkSubmissionReceipt) == 88U);

struct NativeGpuSdkSnapshot final {
    NativeGpuSdkProbe probe;
    NativeGpuSdkConfig config;
    GpuSurfaceDescriptor surface;
    std::uint64_t initialized_devices{0};
    std::uint64_t configured_surfaces{0};
    std::uint64_t acquired_images{0};
    std::uint64_t submitted_frames{0};
    std::uint64_t retired_frames{0};
    std::uint64_t device_lost_events{0};
    std::uint64_t stale_rejections{0};
    std::uint64_t current_staging_bytes{0};
    std::uint64_t peak_staging_bytes{0};
    std::uint64_t current_device_local_bytes{0};
    std::uint64_t peak_device_local_bytes{0};
    std::uint64_t last_submitted_fence_value{0};
    std::uint64_t completed_fence_value{0};
    std::uint32_t configured_image_count{0};
    std::uint32_t in_flight_frame_count{0};
};

const char* native_gpu_sdk_error_kind_name(NativeGpuSdkErrorKind kind) noexcept;

class NativeGpuSdkApi {
public:
    virtual ~NativeGpuSdkApi() = default;

    virtual NativeGpuApiKind kind() const noexcept = 0;
    virtual NativeGpuSdkProbe probe() noexcept = 0;
    virtual bool initialize(
        const NativeGpuSdkConfig& config,
        NativeGpuSdkError* error) noexcept = 0;
    virtual bool configure_offscreen_surface(
        const GpuSurfaceDescriptor& surface,
        std::uint32_t image_count,
        NativeGpuSdkError* error) noexcept = 0;
    virtual bool acquire_image(
        const GpuSurfaceDescriptor& surface,
        std::uint64_t ticket_id,
        NativePlatformSwapchainImage* image,
        NativeAcquireStatus* status,
        NativeGpuSdkError* error) noexcept = 0;
    virtual bool execute_submission(
        const NativePlatformSubmission& submission,
        NativeGpuSdkSubmissionReceipt* receipt,
        NativeGpuSdkError* error) noexcept = 0;
    virtual bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeGpuSdkError* error) noexcept = 0;
    virtual NativeGpuSdkSnapshot snapshot() const noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

class ReferenceNativeGpuSdkApi final : public NativeGpuSdkApi {
public:
    explicit ReferenceNativeGpuSdkApi(NativeGpuApiKind kind) noexcept;

    NativeGpuApiKind kind() const noexcept override;
    NativeGpuSdkProbe probe() noexcept override;
    bool initialize(
        const NativeGpuSdkConfig& config,
        NativeGpuSdkError* error) noexcept override;
    bool configure_offscreen_surface(
        const GpuSurfaceDescriptor& surface,
        std::uint32_t image_count,
        NativeGpuSdkError* error) noexcept override;
    bool acquire_image(
        const GpuSurfaceDescriptor& surface,
        std::uint64_t ticket_id,
        NativePlatformSwapchainImage* image,
        NativeAcquireStatus* status,
        NativeGpuSdkError* error) noexcept override;
    bool execute_submission(
        const NativePlatformSubmission& submission,
        NativeGpuSdkSubmissionReceipt* receipt,
        NativeGpuSdkError* error) noexcept override;
    bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeGpuSdkError* error) noexcept override;
    NativeGpuSdkSnapshot snapshot() const noexcept override;
    void shutdown() noexcept override;

    void set_next_acquire_status(NativeAcquireStatus status) noexcept;
    void set_next_present_status(NativePresentStatus status) noexcept;
    void set_fail_initialization(bool fail) noexcept;

private:
    mutable std::mutex mutex_;
    NativeGpuSdkSnapshot snapshot_;
    NativeGpuApiKind kind_{NativeGpuApiKind::ReferenceCpu};
    std::uint32_t next_image_index_{0};
    std::uint64_t next_image_generation_{1};
    std::uint64_t next_resource_id_{1};
    std::uint64_t next_fence_value_{1};
    NativeAcquireStatus next_acquire_status_{NativeAcquireStatus::Acquired};
    NativePresentStatus next_present_status_{NativePresentStatus::Presented};
    bool fail_initialization_{false};
    bool initialized_{false};
};

class NativeGpuSdkPlatformDriver final : public NativePlatformDriver {
public:
    NativeGpuSdkPlatformDriver(
        NativeGpuSdkApi* api,
        NativeGpuSdkConfig config) noexcept;

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

    bool retire_completed(
        std::uint64_t completed_fence_value,
        NativeGpuSdkError* error) noexcept;
    NativeGpuSdkSnapshot snapshot() const noexcept;
    void shutdown() noexcept;

private:
    mutable std::mutex mutex_;
    NativeGpuSdkApi* api_{nullptr};
    NativeGpuSdkConfig config_;
    NativePlatformCapabilities capabilities_;
    GpuSurfaceDescriptor surface_;
    bool initialized_{false};
    bool configured_{false};
};

std::unique_ptr<NativeGpuSdkApi> make_vulkan_native_gpu_sdk_api() noexcept;
std::unique_ptr<NativeGpuSdkApi> make_metal_native_gpu_sdk_api() noexcept;
std::unique_ptr<NativeGpuSdkApi> make_direct3d12_native_gpu_sdk_api() noexcept;

bool native_gpu_sdk_build_has_backend(NativeGpuApiKind kind) noexcept;

NativeGpuSdkLimits default_native_gpu_sdk_limits(NativeGpuApiKind kind) noexcept;

} // namespace zevryon::text
