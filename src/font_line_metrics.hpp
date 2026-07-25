#pragma once

#include "catalog_harfbuzz_shaper.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class FontLineMetricSource : std::uint8_t {
    Os2Typographic = 0,
    HorizontalHeader,
    Os2TypographicFallback
};

enum FontLineMetricFlags : std::uint8_t {
    kFontLineMetricHasOs2 = 1U << 0U,
    kFontLineMetricHasHhea = 1U << 1U,
    kFontLineMetricUseTypoMetrics = 1U << 2U,
    kFontLineMetricNegativeLineGap = 1U << 3U
};

struct FontLineMetricRecord final {
    FontFaceId face_id{kInvalidFontFaceId};
    std::uint32_t units_per_em{0};
    std::int32_t ascender{0};
    std::int32_t descender{0};
    std::int32_t line_gap{0};
    std::uint32_t win_ascent{0};
    std::uint32_t win_descent{0};
    FontLineMetricSource source{FontLineMetricSource::HorizontalHeader};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};

    bool operator==(const FontLineMetricRecord&) const noexcept = default;
};

static_assert(
    sizeof(FontLineMetricRecord) == 32U,
    "font-line metric records must remain within the Z2 memory contract");

class FontLineMetricTable final {
public:
    explicit FontLineMetricTable(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    FontLineMetricTable(const FontLineMetricTable&) = delete;
    FontLineMetricTable& operator=(const FontLineMetricTable&) = delete;
    FontLineMetricTable(FontLineMetricTable&&) noexcept = default;
    FontLineMetricTable& operator=(FontLineMetricTable&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;

    // Sorted strictly by face_id. Exactly one record is retained per source
    // binding. Records contain copied scalar metrics and do not retain font bytes.
    std::pmr::vector<FontLineMetricRecord> records;
};

enum class FontLineMetricErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    InvalidBindingTable,
    GenerationMismatch,
    MissingHeadTable,
    InvalidHeadTable,
    InvalidOs2Table,
    InvalidHheaTable,
    InvalidMetrics,
    OutputBudgetExceeded,
    AggregateOverflow
};

struct FontLineMetricError final {
    FontLineMetricErrorKind kind{FontLineMetricErrorKind::None};
    std::size_t binding_index{0};
    FontFaceId face_id{kInvalidFontFaceId};
    std::uint32_t table_tag{0};
    std::size_t byte_offset{0};
    std::string message;
};

struct FontLineMetricStats final {
    std::uint64_t generation_id{0};
    std::uint64_t input_bindings{0};
    std::uint64_t output_records{0};
    std::uint64_t os2_typographic_records{0};
    std::uint64_t hhea_records{0};
    std::uint64_t os2_fallback_records{0};
    std::uint64_t use_typo_metrics_records{0};
    std::uint64_t negative_line_gap_records{0};
    std::uint64_t maximum_design_line_height{0};
    std::uint32_t maximum_units_per_em{0};
};

const char* font_line_metric_source_name(FontLineMetricSource source) noexcept;
const char* font_line_metric_error_kind_name(FontLineMetricErrorKind kind) noexcept;

// Parses one structurally valid sfnt face view. No allocation is performed.
// The selected source is OS/2 sTypo* when USE_TYPO_METRICS is set, otherwise
// hhea; a valid OS/2 sTypo* triple is the final deterministic fallback.
bool read_sfnt_font_line_metric_record(
    FontFaceId face_id,
    const SfntResourceView& view,
    FontLineMetricRecord* output,
    FontLineMetricError* error) noexcept;

// Convenience boundary for the verified-resource path used by catalog bindings.
bool read_font_line_metric_record(
    FontFaceId face_id,
    const VerifiedFontResource& resource,
    FontLineMetricRecord* output,
    FontLineMetricError* error) noexcept;

// Builds one exact-size sorted table from immutable catalog bindings. All
// bindings must be valid, strictly ordered by face_id and belong to one catalog
// generation. Output is empty after every failure.
bool build_font_line_metric_table(
    std::span<const CatalogFontFaceBinding> bindings,
    FontLineMetricTable* output,
    FontLineMetricStats* stats,
    FontLineMetricError* error) noexcept;

const FontLineMetricRecord* find_font_line_metric(
    const FontLineMetricTable& table,
    FontFaceId face_id) noexcept;

} // namespace zevryon::text
