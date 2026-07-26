#pragma once

#include "glyph_atlas_submission.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class DeviceRasterHintingPolicy : std::uint8_t { None = 0, Light, Full };
enum class DeviceRasterLcdFilter : std::uint8_t { None = 0, Default, Light };
enum class DeviceColorGlyphPolicy : std::uint8_t { Reject = 0, BgraFallback };
enum class DeviceRasterBackendKind : std::uint8_t { ReferenceCpu = 0, Platform };

struct DeviceRasterPolicy final {
    std::uint32_t policy_id{0};
    std::uint32_t device_scale_x_16_16{65'536U};
    std::uint32_t device_scale_y_16_16{65'536U};
    std::uint32_t maximum_dimension{256U};
    std::uint64_t maximum_glyph_bytes{1U << 20U};
    std::uint8_t grayscale_phase_count{4U};
    std::uint8_t lcd_phase_count{3U};
    DeviceRasterHintingPolicy hinting{DeviceRasterHintingPolicy::Light};
    DeviceRasterLcdFilter lcd_filter{DeviceRasterLcdFilter::Default};
    DeviceColorGlyphPolicy color_policy{DeviceColorGlyphPolicy::BgraFallback};
    std::uint8_t reserved[3]{0, 0, 0};
};

static_assert(sizeof(DeviceRasterPolicy) == 32U);

struct DeviceRasterFaceSource final {
    std::uint64_t font_generation_id{0};
    FontFaceId face_id{kInvalidFontFaceId};
    std::uint32_t reserved{0};
    std::uint64_t resource_id{0};
    std::span<const std::byte> bytes;
};

enum DeviceGlyphRasterJobFlags : std::uint32_t {
    kDeviceGlyphRasterJobGrayscale = 1U << 0U,
    kDeviceGlyphRasterJobLcd = 1U << 1U,
    kDeviceGlyphRasterJobColor = 1U << 2U
};

struct DeviceGlyphRasterJob final {
    GlyphRasterKey key;
    std::uint64_t queue_generation{0};
    std::uint64_t job_id{0};
    std::uint64_t face_resource_id{0};
    std::int32_t device_x_scale{0};
    std::int32_t device_y_scale{0};
    std::uint32_t working_set_key_index{0};
    std::uint32_t face_source_index{0};
    std::uint32_t flags{0};
    std::uint32_t reserved{0};
};

static_assert(sizeof(DeviceGlyphRasterJob) == 80U);

class DeviceGlyphRasterPlan final {
public:
    explicit DeviceGlyphRasterPlan(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    DeviceGlyphRasterPlan(const DeviceGlyphRasterPlan&) = delete;
    DeviceGlyphRasterPlan& operator=(const DeviceGlyphRasterPlan&) = delete;
    DeviceGlyphRasterPlan(DeviceGlyphRasterPlan&&) noexcept = default;
    DeviceGlyphRasterPlan& operator=(DeviceGlyphRasterPlan&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    std::uint64_t queue_generation{0};
    std::uint64_t atlas_generation_id{0};
    std::pmr::vector<DeviceGlyphRasterJob> jobs;
};

struct DeviceGlyphRasterPlanLimits final {
    std::uint32_t maximum_jobs{0};
};

struct DeviceGlyphRasterPlanRequest final {
    const GlyphRasterWorkingSet* working_set{nullptr};
    std::span<const GlyphRasterKey> resident_keys;
    std::span<const DeviceRasterFaceSource> face_sources;
    DeviceRasterPolicy policy;
    std::uint64_t queue_generation{0};
    std::uint64_t atlas_generation_id{0};
    DeviceGlyphRasterPlanLimits limits;
};

enum class DeviceGlyphRasterPlanErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    MissingFaceSource,
    GenerationMismatch,
    ScaleOverflow,
    JobLimitExceeded,
    OutputBudgetExceeded,
    AggregateOverflow
};

struct DeviceGlyphRasterPlanError final {
    DeviceGlyphRasterPlanErrorKind kind{DeviceGlyphRasterPlanErrorKind::None};
    std::size_t key_index{0};
    FontFaceId face_id{kInvalidFontFaceId};
    std::string message;
};

struct DeviceGlyphRasterPlanStats final {
    std::uint64_t input_unique_keys{0};
    std::uint64_t cache_hits{0};
    std::uint64_t cold_jobs{0};
    std::uint64_t grayscale_jobs{0};
    std::uint64_t lcd_jobs{0};
    std::uint64_t color_jobs{0};
    std::uint64_t referenced_face_sources{0};
};

const char* device_glyph_raster_plan_error_kind_name(
    DeviceGlyphRasterPlanErrorKind kind) noexcept;

bool build_device_glyph_raster_plan(
    const DeviceGlyphRasterPlanRequest& request,
    DeviceGlyphRasterPlan* output,
    DeviceGlyphRasterPlanStats* stats,
    DeviceGlyphRasterPlanError* error) noexcept;

struct DeviceGlyphRasterMetrics final {
    std::uint64_t payload_size{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t row_bytes{0};
    std::int32_t bearing_x{0};
    std::int32_t bearing_y{0};
    GlyphRasterFormat format{GlyphRasterFormat::Alpha8};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};
};

static_assert(sizeof(DeviceGlyphRasterMetrics) == 32U);

enum DeviceGlyphRasterMetricsFlags : std::uint8_t {
    kDeviceGlyphRasterMetricsEmpty = 1U << 0U,
    kDeviceGlyphRasterMetricsColor = 1U << 1U
};

enum class DeviceGlyphRasterBackendErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    UnsupportedMode,
    InvalidFontSource,
    QueryFailed,
    RenderFailed,
    OutputTooSmall,
    ArithmeticOverflow
};

struct DeviceGlyphRasterBackendError final {
    DeviceGlyphRasterBackendErrorKind kind{
        DeviceGlyphRasterBackendErrorKind::None};
    std::string message;
};

class DeviceGlyphRasterBackend {
public:
    virtual ~DeviceGlyphRasterBackend() = default;
    virtual DeviceRasterBackendKind kind() const noexcept = 0;
    virtual bool query(
        const DeviceGlyphRasterJob& job,
        const DeviceRasterFaceSource& face,
        const DeviceRasterPolicy& policy,
        DeviceGlyphRasterMetrics* metrics,
        DeviceGlyphRasterBackendError* error) noexcept = 0;
    virtual bool render(
        const DeviceGlyphRasterJob& job,
        const DeviceRasterFaceSource& face,
        const DeviceRasterPolicy& policy,
        const DeviceGlyphRasterMetrics& metrics,
        std::span<std::byte> destination,
        DeviceGlyphRasterBackendError* error) noexcept = 0;
};

class ReferenceDeviceGlyphRasterBackend final : public DeviceGlyphRasterBackend {
public:
    DeviceRasterBackendKind kind() const noexcept override;
    bool query(
        const DeviceGlyphRasterJob& job,
        const DeviceRasterFaceSource& face,
        const DeviceRasterPolicy& policy,
        DeviceGlyphRasterMetrics* metrics,
        DeviceGlyphRasterBackendError* error) noexcept override;
    bool render(
        const DeviceGlyphRasterJob& job,
        const DeviceRasterFaceSource& face,
        const DeviceRasterPolicy& policy,
        const DeviceGlyphRasterMetrics& metrics,
        std::span<std::byte> destination,
        DeviceGlyphRasterBackendError* error) noexcept override;
};

class DeviceGlyphRasterSourceSet final {
public:
    explicit DeviceGlyphRasterSourceSet(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    DeviceGlyphRasterSourceSet(const DeviceGlyphRasterSourceSet&) = delete;
    DeviceGlyphRasterSourceSet& operator=(const DeviceGlyphRasterSourceSet&) = delete;
    DeviceGlyphRasterSourceSet(DeviceGlyphRasterSourceSet&&) noexcept = default;
    DeviceGlyphRasterSourceSet& operator=(DeviceGlyphRasterSourceSet&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    std::uint64_t queue_generation{0};
    std::pmr::vector<GlyphRasterSourceRecord> sources;
    std::pmr::vector<std::byte> payload;
};

struct DeviceGlyphRasterExecutionLimits final {
    std::uint64_t maximum_payload_bytes{0};
    std::uint32_t maximum_sources{0};
};

struct DeviceGlyphRasterExecutionRequest final {
    const DeviceGlyphRasterPlan* plan{nullptr};
    std::span<const DeviceRasterFaceSource> face_sources;
    DeviceRasterPolicy policy;
    std::uint64_t expected_queue_generation{0};
    DeviceGlyphRasterExecutionLimits limits;
};

enum class DeviceGlyphRasterExecutionErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    StaleQueueGeneration,
    MissingFaceSource,
    BackendQueryFailed,
    BackendRenderFailed,
    InvalidMetrics,
    PayloadLimitExceeded,
    SourceLimitExceeded,
    OutputBudgetExceeded,
    ArithmeticOverflow,
    AggregateOverflow
};

struct DeviceGlyphRasterExecutionError final {
    DeviceGlyphRasterExecutionErrorKind kind{
        DeviceGlyphRasterExecutionErrorKind::None};
    std::size_t job_index{0};
    DeviceGlyphRasterBackendError backend_error;
    std::string message;
};

struct DeviceGlyphRasterExecutionStats final {
    std::uint64_t input_jobs{0};
    std::uint64_t output_sources{0};
    std::uint64_t output_payload_bytes{0};
    std::uint64_t empty_glyphs{0};
    std::uint64_t grayscale_sources{0};
    std::uint64_t lcd_sources{0};
    std::uint64_t color_sources{0};
    std::uint64_t maximum_source_bytes{0};
};

const char* device_glyph_raster_execution_error_kind_name(
    DeviceGlyphRasterExecutionErrorKind kind) noexcept;

bool execute_device_glyph_raster_plan(
    const DeviceGlyphRasterExecutionRequest& request,
    DeviceGlyphRasterBackend* backend,
    DeviceGlyphRasterSourceSet* output,
    DeviceGlyphRasterExecutionStats* stats,
    DeviceGlyphRasterExecutionError* error) noexcept;

struct GlyphAtlasBackendUploadBatch final {
    std::uint64_t atlas_generation_id{0};
    std::uint64_t page_generation{0};
    std::uint32_t page_index{0};
    std::uint32_t first_upload{0};
    std::uint32_t upload_count{0};
    GlyphRasterFormat format{GlyphRasterFormat::Alpha8};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};
};

static_assert(sizeof(GlyphAtlasBackendUploadBatch) == 32U);

enum class GlyphAtlasUploadReceiptStatus : std::uint8_t {
    Pending = 0,
    Submitted,
    Completed
};

struct GlyphAtlasUploadReceipt final {
    std::uint64_t ticket_id{0};
    std::uint64_t fence_value{0};
    std::uint64_t atlas_generation_id{0};
    std::uint64_t page_generation{0};
    std::uint32_t page_index{0};
    std::uint32_t first_upload{0};
    std::uint32_t upload_count{0};
    GlyphAtlasUploadReceiptStatus status{GlyphAtlasUploadReceiptStatus::Pending};
    std::uint8_t reserved[3]{0, 0, 0};
};

static_assert(sizeof(GlyphAtlasUploadReceipt) == 48U);

class GlyphAtlasUploadExecution final {
public:
    explicit GlyphAtlasUploadExecution(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    GlyphAtlasUploadExecution(const GlyphAtlasUploadExecution&) = delete;
    GlyphAtlasUploadExecution& operator=(const GlyphAtlasUploadExecution&) = delete;
    GlyphAtlasUploadExecution(GlyphAtlasUploadExecution&&) noexcept = default;
    GlyphAtlasUploadExecution& operator=(GlyphAtlasUploadExecution&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    std::uint64_t atlas_generation_id{0};
    std::uint64_t submission_epoch{0};
    std::uint64_t last_fence_value{0};
    std::pmr::vector<GlyphAtlasBackendUploadBatch> batches;
    std::pmr::vector<GlyphAtlasUploadReceipt> receipts;
};

enum class GlyphAtlasUploadBackendErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    InvalidPayload,
    SubmissionFailed,
    FenceOverflow
};

struct GlyphAtlasUploadBackendError final {
    GlyphAtlasUploadBackendErrorKind kind{
        GlyphAtlasUploadBackendErrorKind::None};
    std::string message;
};

class GlyphAtlasUploadBackend {
public:
    virtual ~GlyphAtlasUploadBackend() = default;
    virtual bool submit(
        const GlyphAtlasBackendUploadBatch& batch,
        std::span<const GlyphAtlasUploadRecord> uploads,
        std::span<const std::byte> payload,
        std::uint64_t ticket_id,
        std::uint64_t* fence_value,
        GlyphAtlasUploadBackendError* error) noexcept = 0;
};

class ReferenceGlyphAtlasUploadBackend final : public GlyphAtlasUploadBackend {
public:
    bool submit(
        const GlyphAtlasBackendUploadBatch& batch,
        std::span<const GlyphAtlasUploadRecord> uploads,
        std::span<const std::byte> payload,
        std::uint64_t ticket_id,
        std::uint64_t* fence_value,
        GlyphAtlasUploadBackendError* error) noexcept override;

private:
    std::uint64_t next_fence_value_{1};
};

struct GlyphAtlasUploadExecutionLimits final {
    std::uint32_t maximum_batches{0};
    std::uint64_t maximum_upload_bytes{0};
};

struct GlyphAtlasUploadExecutionRequest final {
    const GlyphAtlasSubmission* submission{nullptr};
    const GlyphAtlasCache* cache{nullptr};
    std::span<const std::byte> raster_payload;
    GlyphAtlasUploadExecutionLimits limits;
};

enum class GlyphAtlasUploadExecutionErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    StaleSubmission,
    InvalidUploadTopology,
    UploadLimitExceeded,
    BackendFailure,
    OutputBudgetExceeded,
    ArithmeticOverflow,
    AggregateOverflow
};

struct GlyphAtlasUploadExecutionError final {
    GlyphAtlasUploadExecutionErrorKind kind{
        GlyphAtlasUploadExecutionErrorKind::None};
    std::size_t upload_index{0};
    std::size_t batch_index{0};
    GlyphAtlasUploadBackendError backend_error;
    std::string message;
};

struct GlyphAtlasUploadExecutionStats final {
    std::uint64_t input_uploads{0};
    std::uint64_t upload_bytes{0};
    std::uint64_t output_batches{0};
    std::uint64_t output_receipts{0};
    std::uint64_t coalesced_uploads{0};
    std::uint64_t maximum_uploads_per_batch{0};
    std::uint64_t last_fence_value{0};
};

const char* glyph_atlas_upload_execution_error_kind_name(
    GlyphAtlasUploadExecutionErrorKind kind) noexcept;

bool execute_glyph_atlas_uploads(
    const GlyphAtlasUploadExecutionRequest& request,
    GlyphAtlasUploadBackend* backend,
    GlyphAtlasUploadExecution* output,
    GlyphAtlasUploadExecutionStats* stats,
    GlyphAtlasUploadExecutionError* error) noexcept;

bool glyph_atlas_upload_execution_is_current(
    const GlyphAtlasCache& cache,
    const GlyphAtlasSubmission& submission,
    const GlyphAtlasUploadExecution& execution) noexcept;

} // namespace zevryon::text
