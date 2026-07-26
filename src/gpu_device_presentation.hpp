#pragma once

#include "gpu_atlas_frame_submission.hpp"
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

enum GpuDeviceTextureHandleFlags : std::uint8_t {
    kGpuDeviceTextureHandleValid = 1U << 0U
};

struct GpuDeviceTextureHandle final {
    std::uint64_t device_generation{0};
    std::uint64_t texture_generation{0};
    std::uint64_t resource_id{0};
    std::uint32_t page_index{0};
    GlyphRasterFormat format{GlyphRasterFormat::Alpha8};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};

    bool operator==(const GpuDeviceTextureHandle&) const noexcept = default;
};

static_assert(sizeof(GpuDeviceTextureHandle) == 32U);

enum GpuSurfaceImageHandleFlags : std::uint32_t {
    kGpuSurfaceImageHandleValid = 1U << 0U
};

struct GpuSurfaceImageHandle final {
    std::uint64_t surface_id{0};
    std::uint64_t surface_generation{0};
    std::uint64_t image_generation{0};
    std::uint32_t image_index{0};
    std::uint32_t flags{0};

    bool operator==(const GpuSurfaceImageHandle&) const noexcept = default;
};

static_assert(sizeof(GpuSurfaceImageHandle) == 32U);

enum class GpuDeviceTextureState : std::uint8_t {
    PendingUpload = 0,
    Resident
};

struct GpuDeviceTextureRecord final {
    GpuDeviceTextureHandle handle;
    std::uint64_t atlas_generation_id{0};
    std::uint64_t page_generation{0};
    std::uint64_t ready_fence_value{0};
    std::uint64_t last_use_fence_value{0};
    std::uint64_t payload_checksum{0};
    std::uint32_t pin_count{0};
    GpuDeviceTextureState state{GpuDeviceTextureState::PendingUpload};
    std::uint8_t reserved[3]{0, 0, 0};

    bool operator==(const GpuDeviceTextureRecord&) const noexcept = default;
};

static_assert(sizeof(GpuDeviceTextureRecord) == 80U);

struct GpuSurfaceImageRecord final {
    GpuSurfaceImageHandle handle;
    std::uint64_t last_submit_fence_value{0};
    std::uint64_t frame_id{0};
    std::uint8_t in_flight{0};
    std::uint8_t reserved[15]{0};

    bool operator==(const GpuSurfaceImageRecord&) const noexcept = default;
};

static_assert(sizeof(GpuSurfaceImageRecord) == 64U);

struct GpuDeviceTexturePin final {
    std::uint64_t frame_id{0};
    std::uint64_t texture_generation{0};
    std::uint64_t resource_id{0};
    std::uint32_t texture_index{0};
    std::uint32_t reserved{0};

    bool operator==(const GpuDeviceTexturePin&) const noexcept = default;
};

static_assert(sizeof(GpuDeviceTexturePin) == 32U);

enum class GpuPresentReceiptStatus : std::uint8_t {
    Submitted = 0,
    Retired
};

struct GpuPresentReceipt final {
    GpuSurfaceImageHandle image;
    std::uint64_t frame_id{0};
    std::uint64_t ticket_id{0};
    std::uint64_t wait_fence_value{0};
    std::uint64_t signal_fence_value{0};
    std::uint64_t command_checksum{0};
    std::uint32_t command_count{0};
    GpuPresentReceiptStatus status{GpuPresentReceiptStatus::Submitted};
    std::uint8_t reserved[3]{0, 0, 0};

    bool operator==(const GpuPresentReceipt&) const noexcept = default;
};

static_assert(sizeof(GpuPresentReceipt) == 80U);

struct GpuDeviceInFlightFrameRecord final {
    GpuPresentReceipt receipt;
    std::uint32_t first_pin{0};
    std::uint32_t pin_count{0};
    std::uint8_t occupied{0};
    std::uint8_t reserved[7]{0};

    bool operator==(const GpuDeviceInFlightFrameRecord&) const noexcept = default;
};

static_assert(sizeof(GpuDeviceInFlightFrameRecord) == 96U);

struct GpuDevicePresentationConfig final {
    std::uint32_t maximum_textures{0};
    std::uint32_t surface_image_count{0};
    std::uint32_t maximum_frames_in_flight{0};
    std::uint32_t maximum_texture_pins{0};
    std::uint32_t atlas_page_width{0};
    std::uint32_t atlas_page_height{0};
    std::uint64_t device_generation{0};
};

enum class GpuDeviceApiErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    ResourceAllocationFailed,
    UploadFailed,
    SurfaceConfigurationFailed,
    PresentFailed,
    FenceOverflow
};

struct GpuDeviceApiError final {
    GpuDeviceApiErrorKind kind{GpuDeviceApiErrorKind::None};
    std::string message;
};

class GpuDeviceApi {
public:
    virtual ~GpuDeviceApi() = default;

    virtual bool create_texture(
        std::uint32_t page_index,
        GlyphRasterFormat format,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t device_generation,
        GpuDeviceTextureHandle* output,
        GpuDeviceApiError* error) noexcept = 0;

    virtual void release_texture(
        const GpuDeviceTextureHandle& texture) noexcept = 0;

    virtual bool upload_texture(
        const GpuDeviceTextureHandle& texture,
        const GlyphAtlasBackendUploadBatch& batch,
        std::span<const GlyphAtlasUploadRecord> uploads,
        std::span<const std::byte> payload,
        std::uint64_t ticket_id,
        std::uint64_t* fence_value,
        std::uint64_t* payload_checksum,
        GpuDeviceApiError* error) noexcept = 0;

    virtual bool configure_surface(
        const GpuSurfaceDescriptor& surface,
        std::uint32_t image_count,
        std::span<GpuSurfaceImageHandle> images,
        GpuDeviceApiError* error) noexcept = 0;

    virtual bool submit_and_present(
        const GpuSurfaceImageHandle& image,
        const GpuFrameSubmission& frame,
        std::span<const GlyphAtlasDrawInstance> draw_instances,
        std::uint64_t ticket_id,
        std::uint64_t wait_fence_value,
        std::uint64_t* signal_fence_value,
        std::uint64_t* command_checksum,
        GpuDeviceApiError* error) noexcept = 0;
};

class ReferenceGpuDeviceApi final : public GpuDeviceApi {
public:
    bool create_texture(
        std::uint32_t page_index,
        GlyphRasterFormat format,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t device_generation,
        GpuDeviceTextureHandle* output,
        GpuDeviceApiError* error) noexcept override;

    void release_texture(
        const GpuDeviceTextureHandle& texture) noexcept override;

    bool upload_texture(
        const GpuDeviceTextureHandle& texture,
        const GlyphAtlasBackendUploadBatch& batch,
        std::span<const GlyphAtlasUploadRecord> uploads,
        std::span<const std::byte> payload,
        std::uint64_t ticket_id,
        std::uint64_t* fence_value,
        std::uint64_t* payload_checksum,
        GpuDeviceApiError* error) noexcept override;

    bool configure_surface(
        const GpuSurfaceDescriptor& surface,
        std::uint32_t image_count,
        std::span<GpuSurfaceImageHandle> images,
        GpuDeviceApiError* error) noexcept override;

    bool submit_and_present(
        const GpuSurfaceImageHandle& image,
        const GpuFrameSubmission& frame,
        std::span<const GlyphAtlasDrawInstance> draw_instances,
        std::uint64_t ticket_id,
        std::uint64_t wait_fence_value,
        std::uint64_t* signal_fence_value,
        std::uint64_t* command_checksum,
        GpuDeviceApiError* error) noexcept override;

private:
    std::uint64_t next_texture_id_{1};
    std::uint64_t next_texture_generation_{1};
    std::uint64_t next_image_generation_{1};
    std::uint64_t next_fence_value_{1};
};

struct GpuDevicePresentationSnapshot final {
    core::ResourceSnapshot metadata;
    GpuDevicePresentationConfig config;
    GpuSurfaceDescriptor surface;
    std::uint64_t completed_fence_value{0};
    std::uint64_t last_submitted_fence_value{0};
    std::uint64_t upload_submissions{0};
    std::uint64_t present_submissions{0};
    std::uint64_t retired_frames{0};
    std::uint64_t texture_allocations{0};
    std::uint64_t texture_reuses{0};
    std::uint64_t texture_evictions{0};
    std::uint64_t surface_reconfigurations{0};
    std::uint64_t stale_rejections{0};
    std::size_t texture_count{0};
    std::size_t surface_image_count{0};
    std::size_t in_flight_frame_count{0};
    std::size_t texture_pin_count{0};
};

class GpuDevicePresentationBackend final
    : public GlyphAtlasUploadBackend,
      public GpuFrameBackend {
public:
    GpuDevicePresentationBackend(
        GpuDeviceApi* api,
        GpuDevicePresentationConfig config,
        std::size_t metadata_hard_limit) noexcept;

    GpuDevicePresentationBackend(const GpuDevicePresentationBackend&) = delete;
    GpuDevicePresentationBackend& operator=(
        const GpuDevicePresentationBackend&) = delete;

    bool submit(
        const GlyphAtlasBackendUploadBatch& batch,
        std::span<const GlyphAtlasUploadRecord> uploads,
        std::span<const std::byte> payload,
        std::uint64_t ticket_id,
        std::uint64_t* fence_value,
        GlyphAtlasUploadBackendError* error) noexcept override;

    GpuFrameBackendKind kind() const noexcept override;

    bool submit(
        const GpuFrameSubmission& frame,
        std::span<const GlyphAtlasDrawInstance> draw_instances,
        std::uint64_t ticket_id,
        std::uint64_t wait_fence_value,
        std::uint64_t* signal_fence_value,
        GpuFrameBackendError* error) noexcept override;

    bool retire_completed(
        std::uint64_t completed_fence_value,
        std::string* error) noexcept;

    bool clear(std::string* error) noexcept;

    bool latest_present_receipt(
        GpuPresentReceipt* output) const noexcept;

    GpuDevicePresentationSnapshot snapshot() const noexcept;

private:
    GpuDevicePresentationSnapshot snapshot_locked() const noexcept;

    mutable std::mutex mutex_;
    GpuDeviceApi* api_{nullptr};
    core::ResourceLedger ledger_;
    core::LedgerMemoryResource metadata_resource_;
    std::pmr::vector<GpuDeviceTextureRecord> textures_;
    std::pmr::vector<GpuSurfaceImageRecord> images_;
    std::pmr::vector<GpuDeviceInFlightFrameRecord> frames_;
    std::pmr::vector<GpuDeviceTexturePin> pins_;
    GpuDevicePresentationConfig config_;
    GpuSurfaceDescriptor surface_;
    std::uint64_t completed_fence_value_{0};
    std::uint64_t last_submitted_fence_value_{0};
    std::uint64_t upload_submissions_{0};
    std::uint64_t present_submissions_{0};
    std::uint64_t retired_frames_{0};
    std::uint64_t texture_allocations_{0};
    std::uint64_t texture_reuses_{0};
    std::uint64_t texture_evictions_{0};
    std::uint64_t surface_reconfigurations_{0};
    std::uint64_t stale_rejections_{0};
    GpuPresentReceipt latest_receipt_;
    bool has_latest_receipt_{false};
};

} // namespace zevryon::text
