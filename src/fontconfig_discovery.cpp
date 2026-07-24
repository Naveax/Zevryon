#include "fontconfig_discovery.hpp"

#include "unicode_script.hpp"

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zevryon::text {
namespace {

constexpr std::size_t kScriptCount = static_cast<std::size_t>(ScriptId::Count);

struct ConfigDeleter {
    void operator()(FcConfig* value) const noexcept {
        if (value != nullptr) {
            FcConfigDestroy(value);
        }
    }
};

struct PatternDeleter {
    void operator()(FcPattern* value) const noexcept {
        if (value != nullptr) {
            FcPatternDestroy(value);
        }
    }
};

struct ObjectSetDeleter {
    void operator()(FcObjectSet* value) const noexcept {
        if (value != nullptr) {
            FcObjectSetDestroy(value);
        }
    }
};

struct FontSetDeleter {
    void operator()(FcFontSet* value) const noexcept {
        if (value != nullptr) {
            FcFontSetDestroy(value);
        }
    }
};

using ConfigPtr = std::unique_ptr<FcConfig, ConfigDeleter>;
using PatternPtr = std::unique_ptr<FcPattern, PatternDeleter>;
using ObjectSetPtr = std::unique_ptr<FcObjectSet, ObjectSetDeleter>;
using FontSetPtr = std::unique_ptr<FcFontSet, FontSetDeleter>;

struct OwnedFace {
    std::string identity;
    std::string family;
    std::uint16_t weight{400};
    std::uint8_t width{5};
    FontSlant slant{FontSlant::Upright};
    ScriptId preferred_script{ScriptId::Zyyy};
    std::uint16_t flags{0};
    std::vector<FontCoverageRange> coverage;
};

void clear_error(FontconfigDiscoveryError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    FontconfigDiscoveryErrorKind kind,
    std::size_t pattern_index,
    const char* message,
    FontconfigDiscoveryError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->pattern_index = pattern_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

std::string_view fc_string(FcChar8* value) noexcept {
    if (value == nullptr) {
        return {};
    }
    return reinterpret_cast<const char*>(value);
}

void append_identity_field(std::string* output, std::string_view value) {
    output->append(std::to_string(value.size()));
    output->push_back(':');
    output->append(value);
    output->push_back('|');
}

std::string make_identity(
    std::string_view sysroot,
    std::string_view file,
    int face_index,
    std::string_view postscript,
    std::string_view variations) {
    std::string identity;
    identity.reserve(
        48U + sysroot.size() + file.size() + postscript.size() + variations.size());
    identity.append("fontconfig|");
    append_identity_field(&identity, sysroot);
    append_identity_field(&identity, file);
    append_identity_field(&identity, std::to_string(face_index));
    append_identity_field(&identity, postscript);
    append_identity_field(&identity, variations);
    return identity;
}

std::uint16_t css_weight(int fontconfig_weight) noexcept {
    const double converted = FcWeightToOpenTypeDouble(fontconfig_weight);
    if (!std::isfinite(converted)) {
        return 400U;
    }
    const long rounded = std::lround(converted);
    return static_cast<std::uint16_t>(
        std::clamp<long>(rounded, 1L, 1000L));
}

std::uint8_t css_width(int width) noexcept {
    if (width <= FC_WIDTH_ULTRACONDENSED) {
        return 1U;
    }
    if (width <= FC_WIDTH_EXTRACONDENSED) {
        return 2U;
    }
    if (width <= FC_WIDTH_CONDENSED) {
        return 3U;
    }
    if (width <= FC_WIDTH_SEMICONDENSED) {
        return 4U;
    }
    if (width < FC_WIDTH_SEMIEXPANDED) {
        return 5U;
    }
    if (width < FC_WIDTH_EXPANDED) {
        return 6U;
    }
    if (width < FC_WIDTH_EXTRAEXPANDED) {
        return 7U;
    }
    if (width < FC_WIDTH_ULTRAEXPANDED) {
        return 8U;
    }
    return 9U;
}

FontSlant font_slant(int slant) noexcept {
    if (slant == FC_SLANT_ITALIC) {
        return FontSlant::Italic;
    }
    if (slant == FC_SLANT_OBLIQUE) {
        return FontSlant::Oblique;
    }
    return FontSlant::Upright;
}

bool append_charset(
    const FcCharSet* charset,
    std::vector<FontCoverageRange>* ranges,
    ScriptId* preferred_script,
    std::uint64_t* codepoint_count) {
    if (charset == nullptr || ranges == nullptr || preferred_script == nullptr ||
        codepoint_count == nullptr) {
        return false;
    }

    std::array<std::uint64_t, kScriptCount> script_counts{};
    FcChar32 page_map[FC_CHARSET_MAP_SIZE]{};
    FcChar32 next = 0U;
    FcChar32 base = FcCharSetFirstPage(charset, page_map, &next);
    bool range_open = false;
    std::uint32_t range_first = 0U;
    std::uint32_t range_last = 0U;

    while (base != FC_CHARSET_DONE) {
        for (std::size_t word_index = 0U;
             word_index < static_cast<std::size_t>(FC_CHARSET_MAP_SIZE);
             ++word_index) {
            const FcChar32 word = page_map[word_index];
            for (unsigned bit = 0U; bit < 32U; ++bit) {
                if ((word & (static_cast<FcChar32>(1U) << bit)) == 0U) {
                    continue;
                }
                const std::uint64_t codepoint_wide =
                    static_cast<std::uint64_t>(base) +
                    static_cast<std::uint64_t>(word_index * 32U + bit);
                if (codepoint_wide > 0x10ffffU) {
                    continue;
                }
                const std::uint32_t codepoint =
                    static_cast<std::uint32_t>(codepoint_wide);
                ++(*codepoint_count);

                const ScriptId script = script_of(codepoint);
                if (!is_neutral_script(script)) {
                    const std::size_t script_index =
                        static_cast<std::size_t>(script);
                    if (script_index < script_counts.size()) {
                        ++script_counts[script_index];
                    }
                }

                if (!range_open) {
                    range_first = codepoint;
                    range_last = codepoint;
                    range_open = true;
                } else if (codepoint == range_last + 1U) {
                    range_last = codepoint;
                } else {
                    ranges->push_back({range_first, range_last});
                    range_first = codepoint;
                    range_last = codepoint;
                }
            }
        }
        base = FcCharSetNextPage(charset, page_map, &next);
    }

    if (range_open) {
        ranges->push_back({range_first, range_last});
    }
    if (ranges->empty()) {
        return false;
    }

    const auto best = std::max_element(script_counts.begin(), script_counts.end());
    if (best != script_counts.end() && *best != 0U) {
        *preferred_script = static_cast<ScriptId>(
            static_cast<std::size_t>(best - script_counts.begin()));
    } else {
        *preferred_script = ScriptId::Zyyy;
    }
    return true;
}

bool same_face(const OwnedFace& left, const OwnedFace& right) noexcept {
    return left.identity == right.identity && left.family == right.family &&
           left.weight == right.weight && left.width == right.width &&
           left.slant == right.slant &&
           left.preferred_script == right.preferred_script &&
           left.flags == right.flags && left.coverage == right.coverage;
}

} // namespace

const char* fontconfig_discovery_error_kind_name(
    FontconfigDiscoveryErrorKind kind) noexcept {
    switch (kind) {
        case FontconfigDiscoveryErrorKind::None:
            return "none";
        case FontconfigDiscoveryErrorKind::InitializationFailed:
            return "initialization_failed";
        case FontconfigDiscoveryErrorKind::EnumerationFailed:
            return "enumeration_failed";
        case FontconfigDiscoveryErrorKind::MissingRequiredProperty:
            return "missing_required_property";
        case FontconfigDiscoveryErrorKind::InvalidNativeValue:
            return "invalid_native_value";
        case FontconfigDiscoveryErrorKind::ConflictingDuplicate:
            return "conflicting_duplicate";
        case FontconfigDiscoveryErrorKind::SnapshotBuildFailed:
            return "snapshot_build_failed";
    }
    return "invalid";
}

bool build_fontconfig_generation(
    std::uint64_t generation_id,
    std::size_t discovery_hard_limit,
    std::size_t catalog_hard_limit,
    std::shared_ptr<const FontCatalogGeneration>* output,
    FontconfigDiscoveryStats* stats,
    FontconfigDiscoveryError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->reset();
    *stats = {};
    clear_error(error);

    try {
        ConfigPtr config(FcInitLoadConfigAndFonts());
        if (!config) {
            return fail(
                FontconfigDiscoveryErrorKind::InitializationFailed,
                0U,
                "Fontconfig failed to load its configuration and font database",
                error);
        }

        PatternPtr query(FcPatternCreate());
        ObjectSetPtr objects(FcObjectSetBuild(
            FC_FILE,
            FC_INDEX,
            FC_FAMILY,
            FC_POSTSCRIPT_NAME,
            FC_FONT_VARIATIONS,
            FC_WEIGHT,
            FC_WIDTH,
            FC_SLANT,
            FC_CHARSET,
            FC_VARIABLE,
            FC_COLOR,
            FC_SPACING,
            nullptr));
        if (!query || !objects) {
            return fail(
                FontconfigDiscoveryErrorKind::InitializationFailed,
                0U,
                "Fontconfig query allocation failed",
                error);
        }

        FontSetPtr set(FcFontList(config.get(), query.get(), objects.get()));
        if (!set) {
            return fail(
                FontconfigDiscoveryErrorKind::EnumerationFailed,
                0U,
                "FcFontList failed",
                error);
        }

        const FcChar8* sysroot_value = FcConfigGetSysRoot(config.get());
        const std::string_view sysroot = sysroot_value == nullptr
            ? std::string_view{}
            : std::string_view(reinterpret_cast<const char*>(sysroot_value));

        std::vector<OwnedFace> owned;
        owned.reserve(static_cast<std::size_t>(std::max(set->nfont, 0)));
        stats->fontconfig_version = static_cast<std::uint64_t>(FcGetVersion());
        stats->patterns_seen = static_cast<std::uint64_t>(std::max(set->nfont, 0));

        for (int pattern_index = 0; pattern_index < set->nfont; ++pattern_index) {
            FcPattern* pattern = set->fonts[pattern_index];
            FcChar8* file_value = nullptr;
            FcChar8* family_value = nullptr;
            FcCharSet* charset = nullptr;
            int face_index = 0;
            if (pattern == nullptr ||
                FcPatternGetString(pattern, FC_FILE, 0, &file_value) !=
                    FcResultMatch ||
                FcPatternGetInteger(pattern, FC_INDEX, 0, &face_index) !=
                    FcResultMatch ||
                FcPatternGetString(pattern, FC_FAMILY, 0, &family_value) !=
                    FcResultMatch ||
                FcPatternGetCharSet(pattern, FC_CHARSET, 0, &charset) !=
                    FcResultMatch) {
                return fail(
                    FontconfigDiscoveryErrorKind::MissingRequiredProperty,
                    static_cast<std::size_t>(pattern_index),
                    "Fontconfig pattern lacks file, index, family, or charset",
                    error);
            }
            if (face_index < 0) {
                return fail(
                    FontconfigDiscoveryErrorKind::InvalidNativeValue,
                    static_cast<std::size_t>(pattern_index),
                    "Fontconfig returned a negative face index",
                    error);
            }

            FcChar8* postscript_value = nullptr;
            FcChar8* variations_value = nullptr;
            int weight_value = FC_WEIGHT_REGULAR;
            int width_value = FC_WIDTH_NORMAL;
            int slant_value = FC_SLANT_ROMAN;
            int spacing_value = FC_PROPORTIONAL;
            FcBool variable_value = FcFalse;
            FcBool color_value = FcFalse;
            static_cast<void>(FcPatternGetString(
                pattern, FC_POSTSCRIPT_NAME, 0, &postscript_value));
            static_cast<void>(FcPatternGetString(
                pattern, FC_FONT_VARIATIONS, 0, &variations_value));
            static_cast<void>(FcPatternGetInteger(
                pattern, FC_WEIGHT, 0, &weight_value));
            static_cast<void>(FcPatternGetInteger(
                pattern, FC_WIDTH, 0, &width_value));
            static_cast<void>(FcPatternGetInteger(
                pattern, FC_SLANT, 0, &slant_value));
            static_cast<void>(FcPatternGetInteger(
                pattern, FC_SPACING, 0, &spacing_value));
            static_cast<void>(FcPatternGetBool(
                pattern, FC_VARIABLE, 0, &variable_value));
            static_cast<void>(FcPatternGetBool(
                pattern, FC_COLOR, 0, &color_value));

            OwnedFace face;
            face.identity = make_identity(
                sysroot,
                fc_string(file_value),
                face_index,
                fc_string(postscript_value),
                fc_string(variations_value));
            face.family.assign(fc_string(family_value));
            face.weight = css_weight(weight_value);
            face.width = css_width(width_value);
            face.slant = font_slant(slant_value);
            if (variable_value == FcTrue) {
                face.flags = static_cast<std::uint16_t>(
                    face.flags | kFontFaceVariable);
                ++stats->variable_faces;
            }
            if (color_value == FcTrue) {
                face.flags = static_cast<std::uint16_t>(
                    face.flags | kFontFaceColor);
                ++stats->color_faces;
            }
            if (spacing_value == FC_MONO || spacing_value == FC_CHARCELL) {
                face.flags = static_cast<std::uint16_t>(
                    face.flags | kFontFaceMonospace);
                ++stats->monospace_faces;
            }
            face.flags = static_cast<std::uint16_t>(
                face.flags | kFontFaceSystem);

            std::uint64_t charset_codepoints = 0U;
            if (!append_charset(
                    charset,
                    &face.coverage,
                    &face.preferred_script,
                    &charset_codepoints)) {
                return fail(
                    FontconfigDiscoveryErrorKind::InvalidNativeValue,
                    static_cast<std::size_t>(pattern_index),
                    "Fontconfig returned an empty or invalid charset",
                    error);
            }
            stats->charset_codepoints += charset_codepoints;
            stats->coverage_ranges +=
                static_cast<std::uint64_t>(face.coverage.size());
            owned.push_back(std::move(face));
        }

        std::sort(
            owned.begin(),
            owned.end(),
            [](const OwnedFace& left, const OwnedFace& right) {
                return left.identity < right.identity;
            });
        std::vector<OwnedFace> unique;
        unique.reserve(owned.size());
        for (OwnedFace& face : owned) {
            if (!unique.empty() && unique.back().identity == face.identity) {
                if (!same_face(unique.back(), face)) {
                    return fail(
                        FontconfigDiscoveryErrorKind::ConflictingDuplicate,
                        unique.size(),
                        "Fontconfig returned conflicting metadata for one identity",
                        error);
                }
                ++stats->duplicate_patterns_skipped;
                continue;
            }
            unique.push_back(std::move(face));
        }

        std::vector<FontDiscoveryFace> views;
        views.reserve(unique.size());
        for (const OwnedFace& face : unique) {
            views.push_back(FontDiscoveryFace{
                face.identity,
                face.family,
                face.weight,
                face.width,
                face.slant,
                face.preferred_script,
                face.flags,
                face.coverage});
        }

        FontDiscoveryError generation_error;
        if (!build_font_catalog_generation(
                generation_id,
                views,
                discovery_hard_limit,
                catalog_hard_limit,
                output,
                &stats->generation,
                &generation_error)) {
            error->kind = FontconfigDiscoveryErrorKind::SnapshotBuildFailed;
            error->generation_error = generation_error.kind;
            error->catalog_error = generation_error.catalog_error;
            error->message = generation_error.message;
            return false;
        }
        stats->faces_emitted = static_cast<std::uint64_t>(unique.size());
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            FontconfigDiscoveryErrorKind::EnumerationFailed,
            0U,
            "Fontconfig adapter allocation failed",
            error);
    } catch (...) {
        return fail(
            FontconfigDiscoveryErrorKind::EnumerationFailed,
            0U,
            "Fontconfig adapter failed",
            error);
    }
}

} // namespace zevryon::text
