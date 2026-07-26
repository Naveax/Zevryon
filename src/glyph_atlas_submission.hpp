#pragma once

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

enum class GlyphRasterMode : std::uint8_t {
    Grayscale = 0,
    Lcd,
    Color
};

enum class GlyphRasterFormat : std::uint8_t {
    Alpha8 = 0,
    LcdRgb8,
    Bgra8,
    Empty
};

struct GlyphRasterConfig final {
    GlyphRasterMode mode{GlyphRasterMode::Grayscale};
    std::uint8_t subpixel_x{0};
    std::uint8_t subpixel_y{0};
    std::uint8_t reserved{0};
};

struct GlyphRasterKey final {
    std::uint64_t font_generation_id{0};
    FontFaceId face_id{kInvalidFontFaceId};
    std::uint32_t glyph_id{0};
    std::int32_t x_scale{0};
    std::int32_t y_scale{0};
    GlyphRasterMode mode{GlyphRasterMode::Grayscale};
    std::uint8_t subpixel_x{0};
    std::uint8_t subpixel_y{0};
    std::uint8_t reserved{0};

    bool operator==(const GlyphRasterKey&) const noexcept = default;
};

static_assert(sizeof(GlyphRasterKey) == 32U);

struct GlyphRasterWorkingSetEntry final {
    GlyphRasterKey key;
    std::uint32_t first_use_index{0};
    std::uint32_t use_count{0};
    std::uint32_t first_segment_index{0};
    std::uint32_t flags{0};

    bool operator==(const GlyphRasterWorkingSetEntry&) const noexcept = default;
};

static_assert(sizeof(GlyphRasterWorkingSetEntry) == 48U);

enum GlyphRasterUseFlags : std::uint32_t {
    kGlyphRasterUseRtl = 1U << 0U,
    kGlyphRasterUseBeforeViewport = 1U << 1U,
    kGlyphRasterUseAfterViewport = 1U << 2U
};

struct GlyphRasterUseRecord final {
    std::int64_t viewport_inline_origin{0};
    std::int64_t viewport_baseline_origin{0};
    std::uint32_t key_index{0};
    std::uint32_t paint_command_index{0};
    std::uint32_t glyph_batch_index{0};
    std::uint32_t glyph_index{0};
    std::uint32_t style_id{0};
    std::uint32_t clip_index{0};
    std::uint32_t source_line_index{0};
    std::uint32_t flags{0};

    bool operator==(const GlyphRasterUseRecord&) const noexcept = default;
};

static_assert(sizeof(GlyphRasterUseRecord) == 48U);

class GlyphRasterWorkingSet final {
public:
    explicit GlyphRasterWorkingSet(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    GlyphRasterWorkingSet(const GlyphRasterWorkingSet&) = delete;
    GlyphRasterWorkingSet& operator=(const GlyphRasterWorkingSet&) = delete;
    GlyphRasterWorkingSet(GlyphRasterWorkingSet&&) noexcept = default;
    GlyphRasterWorkingSet& operator=(GlyphRasterWorkingSet&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    std::pmr::vector<GlyphRasterWorkingSetEntry> entries;
    std::pmr::vector<GlyphRasterUseRecord> uses;
};

struct GlyphRasterWorkingSetLimits final {
    std::uint32_t maximum_unique_keys{0};
    std::uint64_t maximum_uses{0};
};

struct GlyphRasterWorkingSetRequest final {
    const TextPaintCommandStream* paint_stream{nullptr};
    const MultiRunShapedText* shaped_text{nullptr};
    std::span<const std::uint64_t> segment_font_generation_ids;
    std::span<const GlyphRasterConfig> segment_raster_configs;
    GlyphRasterWorkingSetLimits limits;
};

enum class GlyphRasterWorkingSetErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    TopologyViolation,
    ArithmeticOverflow,
    WorkingSetLimitExceeded,
    OutputBudgetExceeded,
    AggregateOverflow
};

struct GlyphRasterWorkingSetError final {
    GlyphRasterWorkingSetErrorKind kind{GlyphRasterWorkingSetErrorKind::None};
    std::size_t command_index{0};
    std::size_t glyph_batch_index{0};
    std::size_t glyph_index{0};
    std::string message;
};

struct GlyphRasterWorkingSetStats final {
    std::uint64_t input_commands{0};
    std::uint64_t input_glyph_batches{0};
    std::uint64_t input_glyph_references{0};
    std::uint64_t output_unique_keys{0};
    std::uint64_t output_uses{0};
    std::uint64_t repeated_uses{0};
    std::uint64_t grayscale_keys{0};
    std::uint64_t lcd_keys{0};
    std::uint64_t color_keys{0};
    std::uint64_t rtl_uses{0};
    std::uint64_t maximum_uses_per_key{0};
};

const char* glyph_raster_working_set_error_kind_name(
    GlyphRasterWorkingSetErrorKind kind) noexcept;

bool build_glyph_raster_working_set(
    const GlyphRasterWorkingSetRequest& request,
    GlyphRasterWorkingSet* output,
    GlyphRasterWorkingSetStats* stats,
    GlyphRasterWorkingSetError* error) noexcept;

enum GlyphRasterSourceFlags : std::uint8_t {
    kGlyphRasterSourceEmpty = 1U << 0U
};

struct GlyphRasterSourceRecord final {
    GlyphRasterKey key;
    std::uint64_t content_checksum{0};
    std::uint64_t payload_offset{0};
    std::uint64_t payload_size{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t row_bytes{0};
    std::int32_t bearing_x{0};
    std::int32_t bearing_y{0};
    GlyphRasterFormat format{GlyphRasterFormat::Alpha8};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};
    std::uint32_t reserved2{0};

    bool operator==(const GlyphRasterSourceRecord&) const noexcept = default;
};

static_assert(sizeof(GlyphRasterSourceRecord) == 88U);

struct GlyphAtlasConfig final {
    std::uint32_t page_width{0};
    std::uint32_t page_height{0};
    std::uint32_t maximum_pages{0};
    std::uint32_t maximum_entries{0};
    std::uint32_t slot_padding{0};
    std::uint32_t reserved{0};
};

struct GlyphAtlasPageRecord final {
    std::uint64_t generation{0};
    std::uint64_t last_use_epoch{0};
    std::uint32_t next_x{0};
    std::uint32_t next_y{0};
    std::uint32_t row_height{0};
    std::uint32_t used_area{0};
    std::uint32_t live_entries{0};
    GlyphRasterFormat format{GlyphRasterFormat::Alpha8};
    std::uint8_t initialized{0};
    std::uint16_t reserved{0};
    std::uint32_t reserved2{0};

    bool operator==(const GlyphAtlasPageRecord&) const noexcept = default;
};

static_assert(sizeof(GlyphAtlasPageRecord) == 48U);

enum GlyphAtlasCacheEntryFlags : std::uint8_t {
    kGlyphAtlasCacheEntryEmpty = 1U << 0U
};

struct GlyphAtlasCacheEntry final {
    GlyphRasterKey key;
    std::uint64_t page_generation{0};
    std::uint64_t last_use_epoch{0};
    std::uint64_t raster_checksum{0};
    std::uint32_t page_index{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t atlas_x{0};
    std::uint32_t atlas_y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::int32_t bearing_x{0};
    std::int32_t bearing_y{0};
    GlyphRasterFormat format{GlyphRasterFormat::Alpha8};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};
    std::uint32_t reserved2[2]{0, 0};

    bool operator==(const GlyphAtlasCacheEntry&) const noexcept = default;
};

static_assert(sizeof(GlyphAtlasCacheEntry) == 96U);

struct GlyphAtlasUploadRecord final {
    std::uint64_t atlas_generation_id{0};
    std::uint64_t page_generation{0};
    std::uint64_t payload_offset{0};
    std::uint64_t payload_size{0};
    std::uint32_t page_index{0};
    std::uint32_t atlas_x{0};
    std::uint32_t atlas_y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t row_bytes{0};
    std::uint32_t working_set_key_index{0};
    GlyphRasterFormat format{GlyphRasterFormat::Alpha8};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};

    bool operator==(const GlyphAtlasUploadRecord&) const noexcept = default;
};

static_assert(sizeof(GlyphAtlasUploadRecord) == 64U);

struct GlyphAtlasDrawInstance final {
    std::int64_t viewport_inline_start{0};
    std::int64_t viewport_block_start{0};
    std::uint64_t atlas_generation_id{0};
    std::uint64_t page_generation{0};
    std::uint32_t page_index{0};
    std::uint32_t atlas_x{0};
    std::uint32_t atlas_y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t style_id{0};
    std::uint32_t clip_index{0};
    std::uint32_t working_set_key_index{0};

    bool operator==(const GlyphAtlasDrawInstance&) const noexcept = default;
};

static_assert(sizeof(GlyphAtlasDrawInstance) == 64U);

enum GlyphAtlasDrawBatchFlags : std::uint32_t {
    kGlyphAtlasDrawBatchCoalesced = 1U << 0U
};

struct GlyphAtlasDrawBatch final {
    std::uint64_t page_generation{0};
    std::uint32_t page_index{0};
    std::uint32_t style_id{0};
    std::uint32_t clip_index{0};
    std::uint32_t first_instance{0};
    std::uint32_t instance_count{0};
    std::uint32_t flags{0};

    bool operator==(const GlyphAtlasDrawBatch&) const noexcept = default;
};

static_assert(sizeof(GlyphAtlasDrawBatch) == 32U);

class GlyphAtlasSubmission final {
public:
    explicit GlyphAtlasSubmission(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    GlyphAtlasSubmission(const GlyphAtlasSubmission&) = delete;
    GlyphAtlasSubmission& operator=(const GlyphAtlasSubmission&) = delete;
    GlyphAtlasSubmission(GlyphAtlasSubmission&&) noexcept = default;
    GlyphAtlasSubmission& operator=(GlyphAtlasSubmission&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    std::uint64_t atlas_generation_id{0};
    std::uint64_t submission_epoch{0};
    std::pmr::vector<GlyphAtlasUploadRecord> uploads;
    std::pmr::vector<GlyphAtlasDrawInstance> draw_instances;
    std::pmr::vector<GlyphAtlasDrawBatch> draw_batches;
};

struct GlyphAtlasSubmissionLimits final {
    std::uint32_t maximum_uploads{0};
    std::uint64_t maximum_upload_bytes{0};
    std::uint64_t maximum_draw_instances{0};
    std::uint32_t maximum_draw_batches{0};
};

struct GlyphAtlasSubmissionRequest final {
    const GlyphRasterWorkingSet* working_set{nullptr};
    std::span<const GlyphRasterSourceRecord> raster_sources;
    std::span<const std::byte> raster_payload;
    GlyphAtlasSubmissionLimits limits;
};

enum class GlyphAtlasSubmissionErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    TopologyViolation,
    InvalidRasterSource,
    RasterSourceNotFound,
    RasterChecksumMismatch,
    RasterKeyCollision,
    AtlasCapacityExceeded,
    StaleCacheEntry,
    ArithmeticOverflow,
    SubmissionLimitExceeded,
    MetadataBudgetExceeded,
    OutputBudgetExceeded,
    AggregateOverflow
};

struct GlyphAtlasSubmissionError final {
    GlyphAtlasSubmissionErrorKind kind{GlyphAtlasSubmissionErrorKind::None};
    std::size_t key_index{0};
    std::size_t use_index{0};
    std::size_t source_index{0};
    std::uint32_t page_index{0};
    std::string message;
};

struct GlyphAtlasSubmissionStats final {
    core::ResourceSnapshot metadata_before;
    core::ResourceSnapshot metadata_after;
    std::uint64_t input_unique_keys{0};
    std::uint64_t input_uses{0};
    std::uint64_t cache_hits{0};
    std::uint64_t cache_misses{0};
    std::uint64_t uploads{0};
    std::uint64_t upload_bytes{0};
    std::uint64_t empty_glyphs{0};
    std::uint64_t draw_instances{0};
    std::uint64_t draw_batches{0};
    std::uint64_t coalesced_instances{0};
    std::uint64_t evicted_entries{0};
    std::uint64_t reset_pages{0};
    std::uint64_t maximum_page_live_entries{0};
    std::uint64_t maximum_instances_per_batch{0};
};

struct GlyphAtlasCacheStats final {
    core::ResourceSnapshot metadata;
    GlyphAtlasConfig config;
    std::uint64_t atlas_generation_id{0};
    std::uint64_t use_epoch{0};
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t uploads{0};
    std::uint64_t evicted_entries{0};
    std::uint64_t reset_pages{0};
    std::uint64_t clears{0};
    std::size_t page_count{0};
    std::size_t entry_count{0};
};

class GlyphAtlasCache final {
public:
    GlyphAtlasCache(
        GlyphAtlasConfig config,
        std::size_t metadata_hard_limit) noexcept;

    GlyphAtlasCache(const GlyphAtlasCache&) = delete;
    GlyphAtlasCache& operator=(const GlyphAtlasCache&) = delete;

    void clear() noexcept;
    GlyphAtlasCacheStats snapshot() const noexcept;

private:
    friend bool prepare_glyph_atlas_submission(
        const GlyphAtlasSubmissionRequest&,
        GlyphAtlasCache*,
        GlyphAtlasSubmission*,
        GlyphAtlasSubmissionStats*,
        GlyphAtlasSubmissionError*) noexcept;
    friend bool glyph_atlas_submission_is_current(
        const GlyphAtlasCache&,
        const GlyphAtlasSubmission&) noexcept;

    GlyphAtlasCacheStats snapshot_locked() const noexcept;

    mutable std::mutex mutex_;
    core::ResourceLedger ledger_;
    core::LedgerMemoryResource metadata_resource_;
    std::pmr::vector<GlyphAtlasPageRecord> pages_;
    std::pmr::vector<GlyphAtlasCacheEntry> entries_;
    GlyphAtlasConfig config_;
    std::uint64_t atlas_generation_id_{1};
    std::uint64_t use_epoch_{0};
    std::uint64_t hits_{0};
    std::uint64_t misses_{0};
    std::uint64_t uploads_{0};
    std::uint64_t evicted_entries_{0};
    std::uint64_t reset_pages_{0};
    std::uint64_t clears_{0};
};

const char* glyph_atlas_submission_error_kind_name(
    GlyphAtlasSubmissionErrorKind kind) noexcept;

bool prepare_glyph_atlas_submission(
    const GlyphAtlasSubmissionRequest& request,
    GlyphAtlasCache* cache,
    GlyphAtlasSubmission* output,
    GlyphAtlasSubmissionStats* stats,
    GlyphAtlasSubmissionError* error) noexcept;

bool glyph_atlas_submission_is_current(
    const GlyphAtlasCache& cache,
    const GlyphAtlasSubmission& submission) noexcept;

} // namespace zevryon::text
