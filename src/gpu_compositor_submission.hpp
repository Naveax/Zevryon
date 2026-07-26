#pragma once

#include "device_raster_backend.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "text_paint_command_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class GpuTextureResidencyState : std::uint8_t {
    Unallocated = 0,
    Pending,
    Resident
};

enum GpuTextureHandleFlags : std::uint8_t {
    kGpuTextureHandleValid = 1U << 0U
};

struct GpuTextureHandle final {
    std::uint64_t device_generation{0};
    std::uint64_t texture_generation{0};
    std::uint64_t texture_id{0};
    std::uint32_t page_index{std::numeric_limits<std::uint32_t>::max()};
    GlyphRasterFormat format{GlyphRasterFormat::Alpha8};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};

    bool operator==(const GpuTextureHandle&) const noexcept = default;
};

static_assert(sizeof(GpuTextureHandle) == 32U);

struct GpuTextureResidencyRecord final {
    GpuTextureHandle handle;
    std::uint64_t atlas_generation_id{0};
    std::uint64_t page_generation{0};
    std::uint64_t last_use_epoch{0};
    std::uint64_t ready_fence_value{0};
    GpuTextureResidencyState state{GpuTextureResidencyState::Unallocated};
    std::uint8_t reserved[7]{0, 0, 0, 0, 0, 0, 0};

    bool operator==(const GpuTextureResidencyRecord&) const noexcept = default;
};

static_assert(sizeof(GpuTextureResidencyRecord) == 72U);

enum GpuTextureUploadCommandFlags : std::uint32_t {
    kGpuTextureUploadRequiresWait = 1U << 0U
};

struct GpuTextureUploadCommand final {
    GpuTextureHandle texture;
    std::uint64_t atlas_generation_id{0};
    std::uint64_t page_generation{0};
    std::uint64_t required_fence_value{0};
    std::uint32_t first_upload{0};
    std::uint32_t upload_count{0};
    std::uint32_t flags{0};
    std::uint32_t reserved{0};

    bool operator==(const GpuTextureUploadCommand&) const noexcept = default;
};

static_assert(sizeof(GpuTextureUploadCommand) == 72U);

enum GpuGlyphDrawPacketFlags : std::uint32_t {
    kGpuGlyphDrawRequiresUploadWait = 1U << 0U,
    kGpuGlyphDrawCoalesced = 1U << 1U
};

struct GpuGlyphDrawPacket final {
    GpuTextureHandle texture;
    std::uint64_t required_fence_value{0};
    std::uint32_t first_instance{0};
    std::uint32_t instance_count{0};
    std::uint32_t style_id{0};
    std::uint32_t clip_index{0};
    std::uint32_t flags{0};
    std::uint32_t reserved{0};

    bool operator==(const GpuGlyphDrawPacket&) const noexcept = default;
};

static_assert(sizeof(GpuGlyphDrawPacket) == 64U);

struct GpuFillRectPacket final {
    std::int64_t viewport_inline_start{0};
    std::int64_t viewport_block_start{0};
    std::uint64_t inline_size{0};
    std::uint64_t block_size{0};
    std::uint32_t style_id{0};
    std::uint32_t source_line_index{0};
    std::uint32_t source_fragment_index{0};
    std::uint32_t flags{0};

    bool operator==(const GpuFillRectPacket&) const noexcept = default;
};

static_assert(sizeof(GpuFillRectPacket) == 48U);

enum class GpuCompositorCommandKind : std::uint32_t {
    SelectionFill = 0,
    GlyphDraw,
    CaretFill
};

struct GpuCompositorCommandRecord final {
    GpuCompositorCommandKind kind{GpuCompositorCommandKind::SelectionFill};
    std::uint32_t payload_index{0};
    std::uint32_t clip_index{0};
    std::uint32_t flags{0};

    bool operator==(const GpuCompositorCommandRecord&) const noexcept = default;
};

static_assert(sizeof(GpuCompositorCommandRecord) == 16U);

class GpuCompositorFrame final {
public:
    explicit GpuCompositorFrame(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    GpuCompositorFrame(const GpuCompositorFrame&) = delete;
    GpuCompositorFrame& operator=(const GpuCompositorFrame&) = delete;
    GpuCompositorFrame(GpuCompositorFrame&&) noexcept = default;
    GpuCompositorFrame& operator=(GpuCompositorFrame&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    std::uint64_t device_generation{0};
    std::uint64_t frame_generation{0};
    std::uint64_t atlas_generation_id{0};
    std::uint64_t required_upload_fence{0};
    std::pmr::vector<TextPaintClipRect> clips;
    std::pmr::vector<GpuTextureUploadCommand> texture_uploads;
    std::pmr::vector<GpuGlyphDrawPacket> glyph_draws;
    std::pmr::vector<GpuFillRectPacket> fill_rects;
    std::pmr::vector<GpuCompositorCommandRecord> commands;
};

struct GpuTextureConfig final {
    std::uint32_t page_width{0};
    std::uint32_t page_height{0};
    std::uint32_t maximum_textures{0};
    std::uint32_t maximum_in_flight_frames{0};
    std::uint64_t device_generation{0};
};

struct GpuCompositorFrameLimits final {
    std::uint32_t maximum_texture_uploads{0};
    std::uint32_t maximum_glyph_draws{0};
    std::uint32_t maximum_fill_rects{0};
    std::uint32_t maximum_commands{0};
};

struct GpuCompositorFrameRequest final {
    const TextPaintCommandStream* paint_stream{nullptr};
    const GlyphAtlasSubmission* atlas_submission{nullptr};
    const GlyphAtlasUploadExecution* upload_execution{nullptr};
    const GlyphAtlasCache* atlas_cache{nullptr};
    std::span<const std::byte> raster_payload;
    std::uint64_t frame_generation{0};
    std::uint64_t completed_upload_fence{0};
    GpuCompositorFrameLimits limits;
};

enum class GpuCompositorBackendErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    AllocationFailed,
    UploadFailed,
    SubmitFailed,
    FenceOverflow
};

struct GpuCompositorBackendError final {
    GpuCompositorBackendErrorKind kind{GpuCompositorBackendErrorKind::None};
    std::string message;
};

class GpuCompositorBackend {
public:
    virtual ~GpuCompositorBackend() = default;
    virtual bool allocate_texture(
        std::uint32_t page_index,
        GlyphRasterFormat format,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t device_generation,
        GpuTextureHandle* output,
        GpuCompositorBackendError* error) noexcept = 0;
    virtual void release_texture(const GpuTextureHandle& texture) noexcept = 0;
    virtual bool encode_uploads(
        const GpuTextureUploadCommand& command,
        std::span<const GlyphAtlasUploadRecord> uploads,
        std::span<const std::byte> payload,
        GpuCompositorBackendError* error) noexcept = 0;
    virtual bool submit_frame(
        const GpuCompositorFrame& frame,
        std::uint64_t frame_id,
        std::uint64_t* fence_value,
        GpuCompositorBackendError* error) noexcept = 0;
};

class ReferenceGpuCompositorBackend final : public GpuCompositorBackend {
public:
    bool allocate_texture(
        std::uint32_t page_index,
        GlyphRasterFormat format,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t device_generation,
        GpuTextureHandle* output,
        GpuCompositorBackendError* error) noexcept override;
    void release_texture(const GpuTextureHandle& texture) noexcept override;
    bool encode_uploads(
        const GpuTextureUploadCommand& command,
        std::span<const GlyphAtlasUploadRecord> uploads,
        std::span<const std::byte> payload,
        GpuCompositorBackendError* error) noexcept override;
    bool submit_frame(
        const GpuCompositorFrame& frame,
        std::uint64_t frame_id,
        std::uint64_t* fence_value,
        GpuCompositorBackendError* error) noexcept override;

private:
    std::uint64_t next_texture_id_{1};
    std::uint64_t next_texture_generation_{1};
    std::uint64_t next_fence_value_{1};
};

struct GpuFrameReceipt final {
    std::uint64_t frame_id{0};
    std::uint64_t frame_generation{0};
    std::uint64_t device_generation{0};
    std::uint64_t fence_value{0};
    std::uint64_t required_upload_fence{0};
    std::uint32_t slot_index{0};
    std::uint32_t flags{0};

    bool operator==(const GpuFrameReceipt&) const noexcept = default;
};

static_assert(sizeof(GpuFrameReceipt) == 48U);

struct GpuInFlightFrameRecord final {
    GpuFrameReceipt receipt;
    std::uint64_t atlas_generation_id{0};
    std::uint8_t occupied{0};
    std::uint8_t reserved[7]{0, 0, 0, 0, 0, 0, 0};

    bool operator==(const GpuInFlightFrameRecord&) const noexcept = default;
};

static_assert(sizeof(GpuInFlightFrameRecord) == 64U);

struct GpuTextureResidencyStats final {
    core::ResourceSnapshot metadata;
    GpuTextureConfig config;
    std::uint64_t residency_epoch{0};
    std::uint64_t next_frame_id{0};
    std::uint64_t allocated_textures{0};
    std::uint64_t reused_textures{0};
    std::uint64_t released_textures{0};
    std::uint64_t evicted_textures{0};
    std::uint64_t submitted_frames{0};
    std::uint64_t retired_frames{0};
    std::uint64_t stale_rejections{0};
    std::size_t texture_count{0};
    std::size_t in_flight_count{0};
};

class GpuTextureResidencyCache final {
public:
    GpuTextureResidencyCache(
        GpuTextureConfig config,
        std::size_t metadata_hard_limit) noexcept;

    GpuTextureResidencyCache(const GpuTextureResidencyCache&) = delete;
    GpuTextureResidencyCache& operator=(const GpuTextureResidencyCache&) = delete;

    void clear(GpuCompositorBackend* backend) noexcept;
    void retire_completed_frames(std::uint64_t completed_fence) noexcept;
    GpuTextureResidencyStats snapshot() const noexcept;

private:
    friend bool prepare_gpu_compositor_frame(
        const GpuCompositorFrameRequest&,
        GpuTextureResidencyCache*,
        GpuCompositorBackend*,
        GpuCompositorFrame*,
        struct GpuCompositorFrameStats*,
        struct GpuCompositorFrameError*) noexcept;
    friend bool submit_gpu_compositor_frame(
        const GpuCompositorFrame&,
        GpuTextureResidencyCache*,
        GpuCompositorBackend*,
        GpuFrameReceipt*,
        struct GpuCompositorFrameError*) noexcept;
    friend bool gpu_compositor_frame_is_current(
        const GpuTextureResidencyCache&,
        const GpuCompositorFrame&) noexcept;

    GpuTextureResidencyStats snapshot_locked() const noexcept;

    mutable std::mutex mutex_;
    core::ResourceLedger ledger_;
    core::LedgerMemoryResource metadata_resource_;
    std::pmr::vector<GpuTextureResidencyRecord> textures_;
    std::pmr::vector<GpuInFlightFrameRecord> in_flight_;
    GpuTextureConfig config_;
    std::uint64_t residency_epoch_{0};
    std::uint64_t next_frame_id_{1};
    std::uint64_t allocated_textures_{0};
    std::uint64_t reused_textures_{0};
    std::uint64_t released_textures_{0};
    std::uint64_t evicted_textures_{0};
    std::uint64_t submitted_frames_{0};
    std::uint64_t retired_frames_{0};
    std::uint64_t stale_rejections_{0};
};

enum class GpuCompositorFrameErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    StaleAtlasSubmission,
    StaleUploadExecution,
    TextureCapacityExceeded,
    InFlightCapacityExceeded,
    MissingTextureResidency,
    UploadTopologyViolation,
    CommandTopologyViolation,
    ArithmeticOverflow,
    FrameLimitExceeded,
    MetadataBudgetExceeded,
    OutputBudgetExceeded,
    BackendFailure,
    AggregateOverflow
};

struct GpuCompositorFrameError final {
    GpuCompositorFrameErrorKind kind{GpuCompositorFrameErrorKind::None};
    std::size_t command_index{0};
    std::size_t upload_index{0};
    std::size_t draw_index{0};
    std::uint32_t page_index{0};
    GpuCompositorBackendError backend_error;
    std::string message;
};

struct GpuCompositorFrameStats final {
    core::ResourceSnapshot metadata_before;
    core::ResourceSnapshot metadata_after;
    std::uint64_t input_paint_commands{0};
    std::uint64_t input_uploads{0};
    std::uint64_t input_draw_batches{0};
    std::uint64_t allocated_textures{0};
    std::uint64_t reused_textures{0};
    std::uint64_t evicted_textures{0};
    std::uint64_t pending_textures{0};
    std::uint64_t resident_textures{0};
    std::uint64_t output_upload_commands{0};
    std::uint64_t output_glyph_draws{0};
    std::uint64_t output_fill_rects{0};
    std::uint64_t output_commands{0};
    std::uint64_t selection_commands{0};
    std::uint64_t caret_commands{0};
    std::uint64_t required_upload_fence{0};
    std::uint64_t maximum_instances_per_draw{0};
};

const char* gpu_compositor_frame_error_kind_name(
    GpuCompositorFrameErrorKind kind) noexcept;

bool prepare_gpu_compositor_frame(
    const GpuCompositorFrameRequest& request,
    GpuTextureResidencyCache* cache,
    GpuCompositorBackend* backend,
    GpuCompositorFrame* output,
    GpuCompositorFrameStats* stats,
    GpuCompositorFrameError* error) noexcept;

bool submit_gpu_compositor_frame(
    const GpuCompositorFrame& frame,
    GpuTextureResidencyCache* cache,
    GpuCompositorBackend* backend,
    GpuFrameReceipt* receipt,
    GpuCompositorFrameError* error) noexcept;

bool gpu_compositor_frame_is_current(
    const GpuTextureResidencyCache& cache,
    const GpuCompositorFrame& frame) noexcept;

} // namespace zevryon::text
