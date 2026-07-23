#pragma once

#include "font_catalog.hpp"
#include "grapheme_segmenter.hpp"
#include "script_run.hpp"
#include "unicode_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class FontFallbackSource : std::uint8_t {
    Primary = 0,
    PreferredFamily,
    ScriptMatch,
    NeutralScript,
    CrossScript,
    Missing
};

struct FontStyleRequest {
    std::uint16_t weight{400};
    std::uint8_t width{5};
    FontSlant slant{FontSlant::Upright};
};

struct FontFallbackRequest {
    FontFaceId primary_face{kInvalidFontFaceId};
    std::span<const FontFaceId> preferred_faces;
    FontStyleRequest style;
};

struct FontFallbackBoundary {
    std::uint32_t cluster_index{0};
    FontFaceId face_id{kInvalidFontFaceId};
    FontFallbackSource source{FontFallbackSource::Missing};
    std::uint8_t reserved0{0};
    std::uint8_t reserved1{0};
    std::uint8_t reserved2{0};

    bool operator==(const FontFallbackBoundary&) const noexcept = default;
};

static_assert(
    sizeof(FontFallbackBoundary) <= 12U,
    "font fallback boundaries must remain within the Z2 memory contract");

enum class FontFallbackErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    IndexOverflow,
    OutputBudgetExceeded
};

struct FontFallbackError {
    FontFallbackErrorKind kind{FontFallbackErrorKind::None};
    std::size_t cluster_index{0};
    std::string message;
};

struct FontFallbackStats {
    std::uint64_t input_codepoints{0};
    std::uint64_t input_clusters{0};
    std::uint64_t catalog_faces{0};
    std::uint64_t output_runs{0};
    std::uint64_t primary_clusters{0};
    std::uint64_t preferred_family_clusters{0};
    std::uint64_t script_match_clusters{0};
    std::uint64_t neutral_script_clusters{0};
    std::uint64_t cross_script_clusters{0};
    std::uint64_t missing_clusters{0};
    std::uint64_t coverage_checks{0};
    std::uint64_t maximum_cluster_codepoints{0};
};

struct FontFallbackPlan {
    explicit FontFallbackPlan(std::pmr::memory_resource* resource);

    // Non-empty input produces one boundary for every font run plus a final
    // sentinel at cluster_count. Missing clusters use kInvalidFontFaceId.
    std::pmr::vector<FontFallbackBoundary> boundaries;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;
};

const char* font_fallback_error_kind_name(FontFallbackErrorKind kind) noexcept;
const char* font_fallback_source_name(FontFallbackSource source) noexcept;

bool build_font_fallback_plan(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const GraphemeBoundary> grapheme_boundaries,
    std::span<const ScriptRunBoundary> script_run_boundaries,
    const FontCatalog& catalog,
    const FontFallbackRequest& request,
    FontFallbackPlan* output,
    FontFallbackStats* stats,
    FontFallbackError* error) noexcept;

} // namespace zevryon::text
