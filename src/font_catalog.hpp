#pragma once

#include "unicode_script.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

using FontFaceId = std::uint32_t;
inline constexpr FontFaceId kInvalidFontFaceId =
    std::numeric_limits<FontFaceId>::max();

enum class FontSlant : std::uint8_t {
    Upright = 0,
    Italic,
    Oblique
};

enum FontFaceFlag : std::uint16_t {
    kFontFaceVariable = 1U << 0U,
    kFontFaceColor = 1U << 1U,
    kFontFaceMonospace = 1U << 2U,
    kFontFaceSystem = 1U << 3U
};

struct FontCoverageRange {
    std::uint32_t first{0};
    std::uint32_t last{0};

    bool operator==(const FontCoverageRange&) const noexcept = default;
};

struct FontFaceSeed {
    std::uint64_t stable_key{0};
    std::uint32_t family_key{0};
    std::uint16_t weight{400};
    std::uint8_t width{5};
    FontSlant slant{FontSlant::Upright};
    ScriptId preferred_script{ScriptId::Zyyy};
    std::uint16_t flags{0};
    std::span<const FontCoverageRange> coverage;
};

struct FontFaceRecord {
    std::uint64_t stable_key{0};
    std::uint32_t family_key{0};
    std::uint32_t coverage_offset{0};
    std::uint32_t coverage_count{0};
    std::uint16_t weight{400};
    ScriptId preferred_script{ScriptId::Zyyy};
    std::uint8_t width{5};
    FontSlant slant{FontSlant::Upright};
    std::uint16_t flags{0};

    bool operator==(const FontFaceRecord&) const noexcept = default;
};

static_assert(
    sizeof(FontFaceRecord) <= 32U,
    "font face records must remain within the Z2 catalog memory contract");

enum class FontCatalogErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    DuplicateStableKey,
    IndexOverflow,
    OutputBudgetExceeded
};

struct FontCatalogError {
    FontCatalogErrorKind kind{FontCatalogErrorKind::None};
    std::size_t face_index{0};
    std::string message;
};

struct FontCatalogStats {
    std::uint64_t input_faces{0};
    std::uint64_t output_faces{0};
    std::uint64_t input_coverage_ranges{0};
    std::uint64_t output_coverage_ranges{0};
    std::uint64_t adjacent_ranges_merged{0};
    std::uint64_t variable_faces{0};
    std::uint64_t color_faces{0};
};

struct FontCatalog {
    explicit FontCatalog(std::pmr::memory_resource* resource);

    std::pmr::vector<FontFaceRecord> faces;
    std::pmr::vector<FontCoverageRange> coverage_ranges;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;
};

const char* font_catalog_error_kind_name(FontCatalogErrorKind kind) noexcept;

bool build_font_catalog(
    std::span<const FontFaceSeed> seeds,
    FontCatalog* output,
    FontCatalogStats* stats,
    FontCatalogError* error) noexcept;

FontFaceId font_face_id_by_stable_key(
    const FontCatalog& catalog,
    std::uint64_t stable_key) noexcept;

bool font_face_covers(
    const FontCatalog& catalog,
    FontFaceId face_id,
    std::uint32_t codepoint) noexcept;

} // namespace zevryon::text
