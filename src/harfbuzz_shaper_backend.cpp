#include "harfbuzz_shaper.hpp"

#include <hb.h>
#include <hb-ot.h>

#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace zevryon::text {
namespace {

static_assert(
    HB_VERSION_ATLEAST(5, 1, 0),
    "Z2B-1 requires HarfBuzz 5.1 or newer");

struct HbBlobDeleter {
    void operator()(hb_blob_t* value) const noexcept {
        if (value != nullptr) {
            hb_blob_destroy(value);
        }
    }
};
struct HbFaceDeleter {
    void operator()(hb_face_t* value) const noexcept {
        if (value != nullptr) {
            hb_face_destroy(value);
        }
    }
};
struct HbFontDeleter {
    void operator()(hb_font_t* value) const noexcept {
        if (value != nullptr) {
            hb_font_destroy(value);
        }
    }
};
struct HbBufferDeleter {
    void operator()(hb_buffer_t* value) const noexcept {
        if (value != nullptr) {
            hb_buffer_destroy(value);
        }
    }
};

using HbBlob = std::unique_ptr<hb_blob_t, HbBlobDeleter>;
using HbFace = std::unique_ptr<hb_face_t, HbFaceDeleter>;
using HbFont = std::unique_ptr<hb_font_t, HbFontDeleter>;
using HbBuffer = std::unique_ptr<hb_buffer_t, HbBufferDeleter>;

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

void clear_error(HarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        error->kind = HarfBuzzShapingErrorKind::None;
        error->input_index = 0U;
        error->message.clear();
    }
}

bool fail(
    HarfBuzzShapingErrorKind kind,
    std::size_t input_index,
    const char* message,
    HarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->input_index = input_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool valid_scalar(std::uint32_t value) noexcept {
    return value <= 0x10ffffU &&
           !(value >= 0xd800U && value <= 0xdfffU);
}

bool valid_direction(ShapingDirection direction) noexcept {
    return direction == ShapingDirection::LeftToRight ||
           direction == ShapingDirection::RightToLeft ||
           direction == ShapingDirection::TopToBottom ||
           direction == ShapingDirection::BottomToTop;
}

hb_direction_t to_hb_direction(ShapingDirection direction) noexcept {
    switch (direction) {
        case ShapingDirection::LeftToRight:
            return HB_DIRECTION_LTR;
        case ShapingDirection::RightToLeft:
            return HB_DIRECTION_RTL;
        case ShapingDirection::TopToBottom:
            return HB_DIRECTION_TTB;
        case ShapingDirection::BottomToTop:
            return HB_DIRECTION_BTT;
    }
    return HB_DIRECTION_INVALID;
}

bool valid_language(std::string_view language) noexcept {
    if (language.empty() ||
        language.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    for (const char raw : language) {
        const unsigned char value = static_cast<unsigned char>(raw);
        const bool alphanumeric =
            (value >= static_cast<unsigned char>('0') &&
             value <= static_cast<unsigned char>('9')) ||
            (value >= static_cast<unsigned char>('A') &&
             value <= static_cast<unsigned char>('Z')) ||
            (value >= static_cast<unsigned char>('a') &&
             value <= static_cast<unsigned char>('z'));
        if (!alphanumeric && value != static_cast<unsigned char>('-')) {
            return false;
        }
    }
    return true;
}

bool validate_request(
    const HarfBuzzShapingRequest& request,
    std::size_t* first_codepoint,
    std::size_t* codepoint_limit,
    HarfBuzzShapingError* error) noexcept {
    if (request.font_bytes.empty() ||
        request.font_bytes.size() > static_cast<std::size_t>(UINT_MAX)) {
        return fail(
            HarfBuzzShapingErrorKind::InvalidInput,
            0U,
            "font data must be non-empty and fit the HarfBuzz blob contract",
            error);
    }
    if (request.codepoints.empty() ||
        request.grapheme_boundaries.size() < 2U) {
        return fail(
            HarfBuzzShapingErrorKind::InvalidInput,
            0U,
            "shaping requires non-empty codepoints and grapheme boundaries",
            error);
    }
    if (request.codepoints.size() > static_cast<std::size_t>(
                                        std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            HarfBuzzShapingErrorKind::IndexOverflow,
            0U,
            "codepoint count exceeds the 32-bit shaping contract",
            error);
    }
    if (request.grapheme_boundaries.front().codepoint_index != 0U ||
        request.grapheme_boundaries.back().codepoint_index !=
            request.codepoints.size()) {
        return fail(
            HarfBuzzShapingErrorKind::InvalidInput,
            0U,
            "grapheme boundaries must cover the complete codepoint stream",
            error);
    }
    for (std::size_t index = 1U;
         index < request.grapheme_boundaries.size();
         ++index) {
        const GraphemeBoundary& previous =
            request.grapheme_boundaries[index - 1U];
        const GraphemeBoundary& current =
            request.grapheme_boundaries[index];
        if (previous.codepoint_index >= current.codepoint_index ||
            previous.source_offset > current.source_offset) {
            return fail(
                HarfBuzzShapingErrorKind::InvalidInput,
                index - 1U,
                "grapheme boundaries must be strictly increasing",
                error);
        }
    }

    const std::size_t cluster_count =
        request.grapheme_boundaries.size() - 1U;
    if (request.first_cluster >= request.cluster_limit ||
        request.cluster_limit > cluster_count) {
        return fail(
            HarfBuzzShapingErrorKind::InvalidInput,
            request.first_cluster,
            "shaping cluster range is empty or outside the grapheme stream",
            error);
    }
    *first_codepoint = request.grapheme_boundaries[
        request.first_cluster].codepoint_index;
    *codepoint_limit = request.grapheme_boundaries[
        request.cluster_limit].codepoint_index;
    if (*first_codepoint >= *codepoint_limit ||
        *codepoint_limit > request.codepoints.size()) {
        return fail(
            HarfBuzzShapingErrorKind::InvalidInput,
            request.first_cluster,
            "shaping cluster range resolves to invalid codepoint bounds",
            error);
    }
    if (*codepoint_limit - *first_codepoint >
        static_cast<std::size_t>(UINT_MAX)) {
        return fail(
            HarfBuzzShapingErrorKind::IndexOverflow,
            request.first_cluster,
            "shaping segment exceeds the HarfBuzz buffer contract",
            error);
    }
    for (std::size_t index = *first_codepoint;
         index < *codepoint_limit;
         ++index) {
        if (!valid_scalar(request.codepoints[index].value)) {
            return fail(
                HarfBuzzShapingErrorKind::InvalidInput,
                index,
                "shaping input contains an invalid Unicode scalar",
                error);
        }
    }

    if (static_cast<std::size_t>(request.script) >=
            static_cast<std::size_t>(ScriptId::Count) ||
        script_short_name(request.script).empty()) {
        return fail(
            HarfBuzzShapingErrorKind::InvalidInput,
            request.first_cluster,
            "shaping request carries an invalid Script",
            error);
    }
    if (!valid_direction(request.direction) ||
        !valid_language(request.language)) {
        return fail(
            HarfBuzzShapingErrorKind::InvalidInput,
            request.first_cluster,
            "shaping direction or BCP 47 language is invalid",
            error);
    }
    if (request.x_scale < 0 || request.y_scale < 0) {
        return fail(
            HarfBuzzShapingErrorKind::InvalidInput,
            request.first_cluster,
            "shaping scales must be zero or positive",
            error);
    }
    if (request.features.size() > static_cast<std::size_t>(UINT_MAX) ||
        request.variations.size() > static_cast<std::size_t>(UINT_MAX)) {
        return fail(
            HarfBuzzShapingErrorKind::IndexOverflow,
            request.first_cluster,
            "feature or variation count exceeds the HarfBuzz contract",
            error);
    }
    for (std::size_t index = 0U; index < request.features.size(); ++index) {
        if (request.features[index].tag == 0U) {
            return fail(
                HarfBuzzShapingErrorKind::InvalidInput,
                index,
                "OpenType feature tags must be non-zero",
                error);
        }
    }
    for (std::size_t index = 0U; index < request.variations.size(); ++index) {
        if (request.variations[index].tag == 0U ||
            !std::isfinite(request.variations[index].value)) {
            return fail(
                HarfBuzzShapingErrorKind::InvalidInput,
                index,
                "variation coordinates require non-zero tags and finite values",
                error);
        }
    }
    return true;
}

bool checked_add(
    std::int64_t left,
    std::int32_t right,
    std::int64_t* output) noexcept {
    const std::int64_t widened = right;
    if ((widened > 0 &&
         left > std::numeric_limits<std::int64_t>::max() - widened) ||
        (widened < 0 &&
         left < std::numeric_limits<std::int64_t>::min() - widened)) {
        return false;
    }
    *output = left + widened;
    return true;
}

std::uint64_t absolute_value(std::int32_t value) noexcept {
    const std::int64_t widened = value;
    return static_cast<std::uint64_t>(widened < 0 ? -widened : widened);
}

} // namespace

ShapedGlyphRun::ShapedGlyphRun(std::pmr::memory_resource* resource)
    : glyphs(resource) {}

std::pmr::memory_resource* ShapedGlyphRun::resource() const noexcept {
    return glyphs.get_allocator().resource();
}

void ShapedGlyphRun::release() noexcept {
    release_vector(&glyphs);
    first_cluster = 0U;
    cluster_limit = 0U;
    face_index = 0U;
    script = ScriptId::Zyyy;
    direction = ShapingDirection::LeftToRight;
    reserved = 0U;
    x_scale = 0;
    y_scale = 0;
}

const char* harfbuzz_shaping_error_kind_name(
    HarfBuzzShapingErrorKind kind) noexcept {
    switch (kind) {
        case HarfBuzzShapingErrorKind::None:
            return "none";
        case HarfBuzzShapingErrorKind::InvalidInput:
            return "invalid_input";
        case HarfBuzzShapingErrorKind::IndexOverflow:
            return "index_overflow";
        case HarfBuzzShapingErrorKind::InvalidFontData:
            return "invalid_font_data";
        case HarfBuzzShapingErrorKind::BackendAllocationFailed:
            return "backend_allocation_failed";
        case HarfBuzzShapingErrorKind::ShapingFailed:
            return "shaping_failed";
        case HarfBuzzShapingErrorKind::InvalidBackendOutput:
            return "invalid_backend_output";
        case HarfBuzzShapingErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

const char* shaping_direction_name(ShapingDirection direction) noexcept {
    switch (direction) {
        case ShapingDirection::LeftToRight:
            return "ltr";
        case ShapingDirection::RightToLeft:
            return "rtl";
        case ShapingDirection::TopToBottom:
            return "ttb";
        case ShapingDirection::BottomToTop:
            return "btt";
    }
    return "invalid";
}

bool shape_harfbuzz_segment(
    const HarfBuzzShapingRequest& request,
    ShapedGlyphRun* output,
    HarfBuzzShapingStats* stats,
    HarfBuzzShapingError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);

    std::size_t first_codepoint = 0U;
    std::size_t codepoint_limit = 0U;
    if (!validate_request(
            request,
            &first_codepoint,
            &codepoint_limit,
            error)) {
        return false;
    }

    try {
        HbBlob blob(hb_blob_create(
            reinterpret_cast<const char*>(request.font_bytes.data()),
            static_cast<unsigned int>(request.font_bytes.size()),
            HB_MEMORY_MODE_READONLY,
            nullptr,
            nullptr));
        if (!blob ||
            static_cast<std::size_t>(hb_blob_get_length(blob.get())) !=
                request.font_bytes.size()) {
            return fail(
                HarfBuzzShapingErrorKind::BackendAllocationFailed,
                request.first_cluster,
                "HarfBuzz failed to create a read-only font blob",
                error);
        }

        HbFace face(hb_face_create(blob.get(), request.face_index));
        if (!face || hb_face_get_glyph_count(face.get()) == 0U) {
            return fail(
                HarfBuzzShapingErrorKind::InvalidFontData,
                request.face_index,
                "font data or face index does not produce a usable HarfBuzz face",
                error);
        }
        const unsigned int units_per_em = hb_face_get_upem(face.get());
        if (units_per_em == 0U ||
            units_per_em > static_cast<unsigned int>(INT_MAX)) {
            return fail(
                HarfBuzzShapingErrorKind::InvalidFontData,
                request.face_index,
                "font units-per-em is outside the shaping contract",
                error);
        }

        HbFont font(hb_font_create(face.get()));
        if (!font) {
            return fail(
                HarfBuzzShapingErrorKind::BackendAllocationFailed,
                request.face_index,
                "HarfBuzz failed to create a font object",
                error);
        }
        hb_ot_font_set_funcs(font.get());
        const int x_scale = request.x_scale == 0
            ? static_cast<int>(units_per_em)
            : request.x_scale;
        const int y_scale = request.y_scale == 0
            ? static_cast<int>(units_per_em)
            : request.y_scale;
        hb_font_set_scale(font.get(), x_scale, y_scale);

        std::vector<hb_variation_t> variations;
        variations.reserve(request.variations.size());
        for (const ShapingVariation variation : request.variations) {
            variations.push_back({
                static_cast<hb_tag_t>(variation.tag),
                variation.value});
        }
        if (!variations.empty()) {
            hb_font_set_variations(
                font.get(),
                variations.data(),
                static_cast<unsigned int>(variations.size()));
        }

        HbBuffer buffer(hb_buffer_create());
        if (!buffer || !hb_buffer_allocation_successful(buffer.get())) {
            return fail(
                HarfBuzzShapingErrorKind::BackendAllocationFailed,
                request.first_cluster,
                "HarfBuzz failed to create an input buffer",
                error);
        }
        hb_buffer_set_content_type(buffer.get(), HB_BUFFER_CONTENT_TYPE_UNICODE);
        hb_buffer_set_cluster_level(
            buffer.get(),
            HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
        hb_buffer_set_direction(buffer.get(), to_hb_direction(request.direction));
        const std::string_view script_name = script_short_name(request.script);
        hb_buffer_set_script(
            buffer.get(),
            hb_script_from_string(
                script_name.data(),
                static_cast<int>(script_name.size())));
        hb_buffer_set_language(
            buffer.get(),
            hb_language_from_string(
                request.language.data(),
                static_cast<int>(request.language.size())));
        hb_buffer_set_replacement_codepoint(buffer.get(), 0xfffdU);

        hb_buffer_flags_t buffer_flags = HB_BUFFER_FLAG_DEFAULT;
        if (request.beginning_of_text) {
            buffer_flags = static_cast<hb_buffer_flags_t>(
                buffer_flags | HB_BUFFER_FLAG_BOT);
        }
        if (request.end_of_text) {
            buffer_flags = static_cast<hb_buffer_flags_t>(
                buffer_flags | HB_BUFFER_FLAG_EOT);
        }
        if (request.produce_unsafe_to_concat) {
            buffer_flags = static_cast<hb_buffer_flags_t>(
                buffer_flags | HB_BUFFER_FLAG_PRODUCE_UNSAFE_TO_CONCAT);
        }
        buffer_flags = static_cast<hb_buffer_flags_t>(
            buffer_flags | HB_BUFFER_FLAG_PRODUCE_SAFE_TO_INSERT_TATWEEL);
        hb_buffer_set_flags(buffer.get(), buffer_flags);

        for (std::uint32_t cluster = request.first_cluster;
             cluster < request.cluster_limit;
             ++cluster) {
            const std::size_t cluster_first =
                request.grapheme_boundaries[cluster].codepoint_index;
            const std::size_t cluster_end =
                request.grapheme_boundaries[cluster + 1U].codepoint_index;
            for (std::size_t index = cluster_first;
                 index < cluster_end;
                 ++index) {
                hb_buffer_add(
                    buffer.get(),
                    request.codepoints[index].value,
                    cluster);
            }
        }
        if (!hb_buffer_allocation_successful(buffer.get())) {
            return fail(
                HarfBuzzShapingErrorKind::BackendAllocationFailed,
                request.first_cluster,
                "HarfBuzz buffer allocation failed while adding codepoints",
                error);
        }

        std::vector<hb_feature_t> features;
        features.reserve(request.features.size());
        for (const ShapingFeature feature : request.features) {
            features.push_back({
                static_cast<hb_tag_t>(feature.tag),
                feature.value,
                HB_FEATURE_GLOBAL_START,
                HB_FEATURE_GLOBAL_END});
        }
        if (hb_shape_full(
                font.get(),
                buffer.get(),
                features.empty() ? nullptr : features.data(),
                static_cast<unsigned int>(features.size()),
                nullptr) == 0 ||
            !hb_buffer_allocation_successful(buffer.get())) {
            return fail(
                HarfBuzzShapingErrorKind::ShapingFailed,
                request.first_cluster,
                "HarfBuzz could not shape the requested segment",
                error);
        }
        if (hb_buffer_get_content_type(buffer.get()) !=
                HB_BUFFER_CONTENT_TYPE_GLYPHS ||
            !hb_buffer_has_positions(buffer.get())) {
            return fail(
                HarfBuzzShapingErrorKind::InvalidBackendOutput,
                request.first_cluster,
                "HarfBuzz returned a buffer without shaped glyph positions",
                error);
        }

        unsigned int info_count = 0U;
        unsigned int position_count = 0U;
        hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(
            buffer.get(),
            &info_count);
        hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(
            buffer.get(),
            &position_count);
        if (info_count == 0U || info_count != position_count ||
            infos == nullptr || positions == nullptr) {
            return fail(
                HarfBuzzShapingErrorKind::InvalidBackendOutput,
                request.first_cluster,
                "HarfBuzz returned inconsistent glyph and position arrays",
                error);
        }

        HarfBuzzShapingStats working_stats;
        working_stats.font_bytes = request.font_bytes.size();
        working_stats.input_codepoints = codepoint_limit - first_codepoint;
        working_stats.input_clusters =
            request.cluster_limit - request.first_cluster;
        working_stats.output_glyphs = info_count;
        working_stats.glyph_count_before_shaping =
            hb_face_get_glyph_count(face.get());
        working_stats.units_per_em = units_per_em;

        ShapedGlyphRun working(output->resource());
        working.glyphs.reserve(info_count);
        for (unsigned int index = 0U; index < info_count; ++index) {
            const hb_glyph_info_t& info = infos[index];
            const hb_glyph_position_t& position = positions[index];
            if (info.cluster < request.first_cluster ||
                info.cluster >= request.cluster_limit) {
                return fail(
                    HarfBuzzShapingErrorKind::InvalidBackendOutput,
                    index,
                    "HarfBuzz returned a cluster outside the requested segment",
                    error);
            }

            std::uint32_t flags = 0U;
            const hb_glyph_flags_t hb_flags =
                hb_glyph_info_get_glyph_flags(&info);
            if ((hb_flags & HB_GLYPH_FLAG_UNSAFE_TO_BREAK) != 0U) {
                flags |= kShapedGlyphUnsafeToBreak;
                ++working_stats.unsafe_to_break_glyphs;
            }
            if ((hb_flags & HB_GLYPH_FLAG_UNSAFE_TO_CONCAT) != 0U) {
                flags |= kShapedGlyphUnsafeToConcat;
                ++working_stats.unsafe_to_concat_glyphs;
            }
            if ((hb_flags & HB_GLYPH_FLAG_SAFE_TO_INSERT_TATWEEL) != 0U) {
                flags |= kShapedGlyphSafeToInsertTatweel;
                ++working_stats.safe_to_insert_tatweel_glyphs;
            }
            if (info.codepoint == 0U) {
                ++working_stats.missing_glyphs;
            }
            if (!checked_add(
                    working_stats.total_x_advance,
                    position.x_advance,
                    &working_stats.total_x_advance) ||
                !checked_add(
                    working_stats.total_y_advance,
                    position.y_advance,
                    &working_stats.total_y_advance)) {
                return fail(
                    HarfBuzzShapingErrorKind::IndexOverflow,
                    index,
                    "shaped advance totals overflow the 64-bit contract",
                    error);
            }
            working_stats.maximum_absolute_offset = std::max(
                working_stats.maximum_absolute_offset,
                std::max(
                    absolute_value(position.x_offset),
                    absolute_value(position.y_offset)));
            working.glyphs.push_back(ShapedGlyph{
                info.codepoint,
                info.cluster,
                position.x_advance,
                position.y_advance,
                position.x_offset,
                position.y_offset,
                flags});
        }

        working.first_cluster = request.first_cluster;
        working.cluster_limit = request.cluster_limit;
        working.face_index = request.face_index;
        working.script = request.script;
        working.direction = request.direction;
        working.x_scale = x_scale;
        working.y_scale = y_scale;

        output->glyphs.swap(working.glyphs);
        output->first_cluster = working.first_cluster;
        output->cluster_limit = working.cluster_limit;
        output->face_index = working.face_index;
        output->script = working.script;
        output->direction = working.direction;
        output->reserved = 0U;
        output->x_scale = working.x_scale;
        output->y_scale = working.y_scale;
        *stats = working_stats;
        return true;
    } catch (const std::bad_alloc&) {
        output->release();
        return fail(
            HarfBuzzShapingErrorKind::OutputBudgetExceeded,
            request.first_cluster,
            "shaped glyph output exceeded its resource budget",
            error);
    } catch (...) {
        output->release();
        return fail(
            HarfBuzzShapingErrorKind::ShapingFailed,
            request.first_cluster,
            "HarfBuzz shaping failed",
            error);
    }
}

} // namespace zevryon::text
