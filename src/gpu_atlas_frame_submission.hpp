#pragma once

#include "device_raster_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class GpuSurfaceFormat : std::uint8_t {
    Bgra8Unorm = 0,
    Rgba8Unorm
};

struct GpuSurfaceDescriptor final {
    std::uint64_t surface_id{0};
    std::uint64_t generation_id{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    GpuSurfaceFormat format{GpuSurfaceFormat::Bgra8Unorm};
    std::uint8_t premultiplied_alpha{1};
    std::uint16_t reserved{0};
    std::uint32_t reserved2{0};

    bool operator==(const GpuSurfaceDescriptor&) const noexcept = default;
};

static_assert(sizeof(GpuSurfaceDescriptor) == 32U);

enum class GpuFrameCommandKind : std::uint32_t {
    FillRect = 0,
    GlyphBatch
};

struct GpuFrameCommandRecord final {
    GpuFrameCommandKind kind{GpuFrameCommandKind::FillRect};
    std::uint32_t payload_index{0};
    std::uint32_t clip_index{0};
    std::uint32_t flags{0};

    bool operator==(const GpuFrameCommandRecord&) const noexcept = default;
};

static_assert(sizeof(GpuFrameCommandRecord) == 16U);

enum GpuFrameGlyphBatchFlags : std::uint32_t {
    kGpuFrameGlyphBatchCoalesced = 1U << 0U
};

struct GpuFrameGlyphBatch final {
    std::uint64_t page_generation{0};
    std::uint32_t page_index{0};
    std::uint32_t first_instance{0};
    std::uint32_t instance_count{0};
    std::uint32_t style_id{0};
    std::uint32_t clip_index{0};
    std::uint32_t page_reference_index{0};
    std::uint32_t flags{0};

    bool operator==(const GpuFrameGlyphBatch&) const noexcept = default;
};

static_assert(sizeof(GpuFrameGlyphBatch) == 40U);

struct GpuFramePageReference final {
    std::uint64_t page_generation{0};
    std::uint64_t required_upload_fence{0};
    std::uint32_t page_index{0};
    std::uint32_t first_batch{0};
    std::uint32_t batch_count{0};
    GlyphRasterFormat format{GlyphRasterFormat::Alpha8};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};

    bool operator==(const GpuFramePageReference&) const noexcept = default;
};

static_assert(sizeof(GpuFramePageReference) == 32U);

class GpuFrameSubmission final {
public:
    explicit GpuFrameSubmission(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    GpuFrameSubmission(const GpuFrameSubmission&) = delete;
    GpuFrameSubmission& operator=(const GpuFrameSubmission&) = delete;
    GpuFrameSubmission(GpuFrameSubmission&&) noexcept = default;
    GpuFrameSubmission& operator=(GpuFrameSubmission&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    GpuSurfaceDescriptor surface;
    std::uint64_t frame_id{0};
    std::uint64_t atlas_generation_id{0};
    std::uint64_t atlas_submission_epoch{0};
    std::uint64_t required_upload_fence{0};
    std::pmr::vector<TextPaintClipRect> clips;
    std::pmr::vector<GpuFrameCommandRecord> commands;
    std::pmr::vector<TextPaintFillRect> fill_rects;
    std::pmr::vector<GpuFrameGlyphBatch> glyph_batches;
    std::pmr::vector<GpuFramePageReference> page_references;
};

struct GpuFrameSubmissionLimits final {
    std::uint32_t maximum_clips{0};
    std::uint32_t maximum_commands{0};
    std::uint32_t maximum_fill_rects{0};
    std::uint32_t maximum_glyph_batches{0};
    std::uint32_t maximum_page_references{0};
    std::uint64_t maximum_referenced_instances{0};
};

struct GpuFrameSubmissionRequest final {
    const TextPaintCommandStream* paint_stream{nullptr};
    const GlyphRasterWorkingSet* working_set{nullptr};
    const GlyphAtlasSubmission* atlas_submission{nullptr};
    const GlyphAtlasUploadExecution* upload_execution{nullptr};
    const GlyphAtlasCache* cache{nullptr};
    GpuSurfaceDescriptor surface;
    std::uint64_t frame_id{0};
    GpuFrameSubmissionLimits limits;
};

enum class GpuFrameSubmissionErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    StaleAtlasSubmission,
    PaintTopologyViolation,
    DrawTopologyViolation,
    PageFormatMismatch,
    MissingUploadCompletion,
    SubmissionLimitExceeded,
    OutputBudgetExceeded,
    ArithmeticOverflow,
    AggregateOverflow
};

struct GpuFrameSubmissionError final {
    GpuFrameSubmissionErrorKind kind{GpuFrameSubmissionErrorKind::None};
    std::size_t command_index{0};
    std::size_t batch_index{0};
    std::size_t instance_index{0};
    std::uint32_t page_index{0};
    std::string message;
};

struct GpuFrameSubmissionStats final {
    std::uint64_t input_paint_commands{0};
    std::uint64_t input_draw_instances{0};
    std::uint64_t input_draw_batches{0};
    std::uint64_t output_clips{0};
    std::uint64_t output_commands{0};
    std::uint64_t output_fill_rects{0};
    std::uint64_t output_glyph_batches{0};
    std::uint64_t output_page_references{0};
    std::uint64_t selection_commands{0};
    std::uint64_t caret_commands{0};
    std::uint64_t referenced_instances{0};
    std::uint64_t coalesced_instances{0};
    std::uint64_t pages_waiting_for_uploads{0};
    std::uint64_t maximum_batches_per_page{0};
    std::uint64_t required_upload_fence{0};
};

const char* gpu_frame_submission_error_kind_name(
    GpuFrameSubmissionErrorKind kind) noexcept;

bool prepare_gpu_frame_submission(
    const GpuFrameSubmissionRequest& request,
    GpuFrameSubmission* output,
    GpuFrameSubmissionStats* stats,
    GpuFrameSubmissionError* error) noexcept;

bool gpu_frame_submission_is_current(
    const GlyphAtlasCache& cache,
    const GlyphAtlasSubmission& atlas_submission,
    const GlyphAtlasUploadExecution& upload_execution,
    const GpuFrameSubmission& frame) noexcept;

struct GpuAtlasResidentPage final {
    std::uint64_t atlas_generation_id{0};
    std::uint64_t page_generation{0};
    std::uint64_t ready_fence_value{0};
    std::uint64_t last_frame_id{0};
    std::uint32_t page_index{0};
    std::uint32_t pin_count{0};
    std::uint32_t submitted_frames{0};
    GlyphRasterFormat format{GlyphRasterFormat::Alpha8};
    std::uint8_t initialized{0};
    std::uint16_t reserved{0};

    bool operator==(const GpuAtlasResidentPage&) const noexcept = default;
};

static_assert(sizeof(GpuAtlasResidentPage) == 48U);

struct GpuFramePagePin final {
    std::uint64_t frame_id{0};
    std::uint64_t atlas_generation_id{0};
    std::uint64_t page_generation{0};
    std::uint32_t page_index{0};
    std::uint32_t resident_page_index{0};

    bool operator==(const GpuFramePagePin&) const noexcept = default;
};

static_assert(sizeof(GpuFramePagePin) == 32U);

struct GpuInFlightFrameRecord final {
    std::uint64_t frame_id{0};
    std::uint64_t ticket_id{0};
    std::uint64_t submit_fence_value{0};
    std::uint64_t atlas_generation_id{0};
    std::uint64_t scheduler_generation{0};
    std::uint32_t first_pin{0};
    std::uint32_t pin_count{0};
    std::uint32_t command_count{0};
    std::uint32_t flags{0};

    bool operator==(const GpuInFlightFrameRecord&) const noexcept = default;
};

static_assert(sizeof(GpuInFlightFrameRecord) == 56U);

enum class GpuFrameReceiptStatus : std::uint8_t {
    Submitted = 0,
    Retired
};

struct GpuFrameReceipt final {
    std::uint64_t frame_id{0};
    std::uint64_t ticket_id{0};
    std::uint64_t submit_fence_value{0};
    std::uint64_t atlas_generation_id{0};
    std::uint64_t scheduler_generation{0};
    std::uint32_t command_count{0};
    std::uint32_t page_reference_count{0};
    GpuFrameReceiptStatus status{GpuFrameReceiptStatus::Submitted};
    std::uint8_t reserved[3]{0, 0, 0};
    std::uint32_t reserved2{0};

    bool operator==(const GpuFrameReceipt&) const noexcept = default;
};

static_assert(sizeof(GpuFrameReceipt) == 56U);

struct GpuAtlasFrameSchedulerConfig final {
    std::uint32_t maximum_resident_pages{0};
    std::uint32_t maximum_frames_in_flight{0};
    std::uint32_t maximum_page_pins{0};
    std::uint32_t reserved{0};
};

struct GpuAtlasFrameSchedulerSnapshot final {
    core::ResourceSnapshot metadata;
    GpuAtlasFrameSchedulerConfig config;
    std::uint64_t scheduler_generation{0};
    std::uint64_t atlas_generation_id{0};
    std::uint64_t next_ticket_id{0};
    std::uint64_t last_submit_fence_value{0};
    std::uint64_t last_completed_fence_value{0};
    std::uint64_t submitted_frames{0};
    std::uint64_t retired_frames{0};
    std::uint64_t page_replacements{0};
    std::size_t resident_page_count{0};
    std::size_t in_flight_frame_count{0};
    std::size_t page_pin_count{0};
};

class GpuAtlasFrameScheduler final {
public:
    GpuAtlasFrameScheduler(
        GpuAtlasFrameSchedulerConfig config,
        std::size_t metadata_hard_limit) noexcept;

    GpuAtlasFrameScheduler(const GpuAtlasFrameScheduler&) = delete;
    GpuAtlasFrameScheduler& operator=(const GpuAtlasFrameScheduler&) = delete;

    bool clear() noexcept;
    GpuAtlasFrameSchedulerSnapshot snapshot() const noexcept;

private:
    friend bool submit_gpu_frame(
        const struct GpuFrameSubmitRequest&,
        class GpuFrameBackend*,
        GpuAtlasFrameScheduler*,
        GpuFrameReceipt*,
        struct GpuFrameSubmitStats*,
        struct GpuFrameSubmitError*) noexcept;
    friend bool retire_gpu_frames(
        GpuAtlasFrameScheduler*,
        std::uint64_t,
        struct GpuFrameRetireStats*,
        struct GpuFrameRetireError*) noexcept;
    friend bool gpu_frame_receipt_is_current(
        const GpuAtlasFrameScheduler&,
        const GpuFrameReceipt&) noexcept;

    GpuAtlasFrameSchedulerSnapshot snapshot_locked() const noexcept;

    mutable std::mutex mutex_;
    core::ResourceLedger ledger_;
    core::LedgerMemoryResource metadata_resource_;
    std::pmr::vector<GpuAtlasResidentPage> pages_;
    std::pmr::vector<GpuInFlightFrameRecord> frames_;
    std::pmr::vector<GpuFramePagePin> pins_;
    GpuAtlasFrameSchedulerConfig config_;
    std::uint64_t scheduler_generation_{1};
    std::uint64_t atlas_generation_id_{0};
    std::uint64_t next_ticket_id_{1};
    std::uint64_t last_submit_fence_value_{0};
    std::uint64_t last_completed_fence_value_{0};
    std::uint64_t submitted_frames_{0};
    std::uint64_t retired_frames_{0};
    std::uint64_t page_replacements_{0};
};

enum class GpuFrameBackendKind : std::uint8_t {
    ReferenceCpu = 0,
    Platform
};

enum class GpuFrameBackendErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    InvalidCommandStream,
    InvalidDrawTopology,
    SubmissionFailed,
    FenceOverflow
};

struct GpuFrameBackendError final {
    GpuFrameBackendErrorKind kind{GpuFrameBackendErrorKind::None};
    std::string message;
};

class GpuFrameBackend {
public:
    virtual ~GpuFrameBackend() = default;
    virtual GpuFrameBackendKind kind() const noexcept = 0;
    virtual bool submit(
        const GpuFrameSubmission& frame,
        std::span<const GlyphAtlasDrawInstance> draw_instances,
        std::uint64_t ticket_id,
        std::uint64_t wait_fence_value,
        std::uint64_t* signal_fence_value,
        GpuFrameBackendError* error) noexcept = 0;
};

class ReferenceGpuFrameBackend final : public GpuFrameBackend {
public:
    GpuFrameBackendKind kind() const noexcept override;
    bool submit(
        const GpuFrameSubmission& frame,
        std::span<const GlyphAtlasDrawInstance> draw_instances,
        std::uint64_t ticket_id,
        std::uint64_t wait_fence_value,
        std::uint64_t* signal_fence_value,
        GpuFrameBackendError* error) noexcept override;

private:
    std::uint64_t next_fence_value_{1};
};

struct GpuFrameSubmitRequest final {
    const GpuFrameSubmission* frame{nullptr};
    const GlyphAtlasSubmission* atlas_submission{nullptr};
    const GlyphAtlasUploadExecution* upload_execution{nullptr};
    const GlyphAtlasCache* cache{nullptr};
};

enum class GpuFrameSubmitErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    StaleFrame,
    SchedulerCapacityExceeded,
    PageNotResident,
    PagePinned,
    PageNotReady,
    BackendFailure,
    MetadataBudgetExceeded,
    ArithmeticOverflow,
    AggregateOverflow
};

struct GpuFrameSubmitError final {
    GpuFrameSubmitErrorKind kind{GpuFrameSubmitErrorKind::None};
    std::size_t page_reference_index{0};
    std::size_t frame_index{0};
    std::uint32_t page_index{0};
    GpuFrameBackendError backend_error;
    std::string message;
};

struct GpuFrameSubmitStats final {
    core::ResourceSnapshot metadata_before;
    core::ResourceSnapshot metadata_after;
    std::uint64_t input_commands{0};
    std::uint64_t input_page_references{0};
    std::uint64_t synchronized_upload_pages{0};
    std::uint64_t reused_resident_pages{0};
    std::uint64_t pinned_pages{0};
    std::uint64_t signal_fence_value{0};
    std::uint64_t frames_in_flight{0};
};

const char* gpu_frame_submit_error_kind_name(
    GpuFrameSubmitErrorKind kind) noexcept;

bool submit_gpu_frame(
    const GpuFrameSubmitRequest& request,
    GpuFrameBackend* backend,
    GpuAtlasFrameScheduler* scheduler,
    GpuFrameReceipt* receipt,
    GpuFrameSubmitStats* stats,
    GpuFrameSubmitError* error) noexcept;

enum class GpuFrameRetireErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    FenceRegression,
    PinTopologyViolation,
    AggregateOverflow
};

struct GpuFrameRetireError final {
    GpuFrameRetireErrorKind kind{GpuFrameRetireErrorKind::None};
    std::size_t frame_index{0};
    std::uint32_t page_index{0};
    std::string message;
};

struct GpuFrameRetireStats final {
    std::uint64_t completed_fence_value{0};
    std::uint64_t retired_frames{0};
    std::uint64_t released_page_pins{0};
    std::uint64_t remaining_frames{0};
    std::uint64_t remaining_page_pins{0};
};

const char* gpu_frame_retire_error_kind_name(
    GpuFrameRetireErrorKind kind) noexcept;

bool retire_gpu_frames(
    GpuAtlasFrameScheduler* scheduler,
    std::uint64_t completed_fence_value,
    GpuFrameRetireStats* stats,
    GpuFrameRetireError* error) noexcept;

bool gpu_frame_receipt_is_current(
    const GpuAtlasFrameScheduler& scheduler,
    const GpuFrameReceipt& receipt) noexcept;

} // namespace zevryon::text
