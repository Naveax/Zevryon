#include "multi_run_harfbuzz_shaper.hpp"

#include "unicode_script.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

std::pmr::memory_resource* usable_resource(
    std::pmr::memory_resource* resource) noexcept {
    return resource != nullptr ? resource : std::pmr::get_default_resource();
}

void clear_error(MultiRunCatalogHarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    MultiRunCatalogHarfBuzzShapingErrorKind kind,
    std::size_t run_index,
    std::uint32_t cluster_index,
    FontFaceId face_id,
    const char* message,
    MultiRunCatalogHarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->run_index = run_index;
        error->cluster_index = cluster_index;
        error->face_id = face_id;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool valid_script(ScriptId script) noexcept {
    return static_cast<std::size_t>(script) <
               static_cast<std::size_t>(ScriptId::Count) &&
           !script_short_name(script).empty();
}

bool valid_fallback_source(FontFallbackSource source) noexcept {
    return static_cast<std::size_t>(source) <=
           static_cast<std::size_t>(FontFallbackSource::Missing);
}

bool checked_add(std::uint64_t* value, std::uint64_t addition) noexcept {
    if (*value > std::numeric_limits<std::uint64_t>::max() - addition) {
        return false;
    }
    *value += addition;
    return true;
}

bool checked_add(std::int64_t* value, std::int64_t addition) noexcept {
    if ((addition > 0 &&
         *value > std::numeric_limits<std::int64_t>::max() - addition) ||
        (addition < 0 &&
         *value < std::numeric_limits<std::int64_t>::min() - addition)) {
        return false;
    }
    *value += addition;
    return true;
}

const CatalogFontFaceBinding* find_binding(
    std::span<const CatalogFontFaceBinding> bindings,
    FontFaceId face_id) noexcept {
    const auto found = std::lower_bound(
        bindings.begin(),
        bindings.end(),
        face_id,
        [](const CatalogFontFaceBinding& binding, FontFaceId value) {
            return binding.face_id() < value;
        });
    return found != bindings.end() && found->face_id() == face_id
               ? &*found
               : nullptr;
}

bool validate_binding_table(
    std::span<const CatalogFontFaceBinding> bindings,
    FontGenerationFingerprint* fingerprint,
    std::uint64_t* generation_id,
    MultiRunCatalogHarfBuzzShapingError* error) noexcept {
    if (bindings.empty()) {
        return fail(
            MultiRunCatalogHarfBuzzShapingErrorKind::InvalidBindingTable,
            0U,
            0U,
            kInvalidFontFaceId,
            "multi-run shaping requires at least one catalog binding",
            error);
    }

    for (std::size_t index = 0U; index < bindings.size(); ++index) {
        const CatalogFontFaceBinding& binding = bindings[index];
        if (!binding.valid() || binding.face_id() == kInvalidFontFaceId) {
            return fail(
                MultiRunCatalogHarfBuzzShapingErrorKind::InvalidBindingTable,
                index,
                0U,
                binding.face_id(),
                "binding table contains an invalid catalog binding",
                error);
        }
        if (index != 0U &&
            bindings[index - 1U].face_id() >= binding.face_id()) {
            return fail(
                MultiRunCatalogHarfBuzzShapingErrorKind::InvalidBindingTable,
                index,
                0U,
                binding.face_id(),
                "binding table must be sorted strictly by face id",
                error);
        }
        if (index == 0U) {
            *fingerprint = binding.generation_fingerprint();
            *generation_id = binding.generation_id();
        } else if (binding.generation_id() != *generation_id ||
                   binding.generation_fingerprint() != *fingerprint) {
            return fail(
                MultiRunCatalogHarfBuzzShapingErrorKind::GenerationMismatch,
                index,
                0U,
                binding.face_id(),
                "all bindings must belong to one immutable catalog generation",
                error);
        }
    }
    return true;
}

bool add_segment_stats(
    const HarfBuzzShapingStats& segment,
    MultiRunCatalogHarfBuzzShapingStats* stats) noexcept {
    return checked_add(&stats->output_glyphs, segment.output_glyphs) &&
           checked_add(&stats->missing_glyphs, segment.missing_glyphs) &&
           checked_add(
               &stats->unsafe_to_break_glyphs,
               segment.unsafe_to_break_glyphs) &&
           checked_add(
               &stats->unsafe_to_concat_glyphs,
               segment.unsafe_to_concat_glyphs) &&
           checked_add(
               &stats->safe_to_insert_tatweel_glyphs,
               segment.safe_to_insert_tatweel_glyphs) &&
           checked_add(&stats->total_x_advance, segment.total_x_advance) &&
           checked_add(&stats->total_y_advance, segment.total_y_advance);
}

} // namespace

MultiRunShapedSegment::MultiRunShapedSegment(
    std::pmr::memory_resource* glyph_resource)
    : glyphs(usable_resource(glyph_resource)) {}

MultiRunShapedText::MultiRunShapedText(std::pmr::memory_resource* resource)
    : segments(usable_resource(resource)),
      glyph_resource_(usable_resource(resource)) {}

MultiRunShapedText::MultiRunShapedText(
    std::pmr::memory_resource* metadata_resource,
    std::pmr::memory_resource* glyph_resource)
    : segments(usable_resource(metadata_resource)),
      glyph_resource_(usable_resource(glyph_resource)) {}

std::pmr::memory_resource* MultiRunShapedText::metadata_resource() const noexcept {
    return segments.get_allocator().resource();
}

std::pmr::memory_resource* MultiRunShapedText::glyph_resource() const noexcept {
    return glyph_resource_;
}

void MultiRunShapedText::release() noexcept {
    release_vector(&segments);
}

const char* multi_run_catalog_harfbuzz_shaping_error_kind_name(
    MultiRunCatalogHarfBuzzShapingErrorKind kind) noexcept {
    switch (kind) {
        case MultiRunCatalogHarfBuzzShapingErrorKind::None:
            return "none";
        case MultiRunCatalogHarfBuzzShapingErrorKind::InvalidArgument:
            return "invalid_argument";
        case MultiRunCatalogHarfBuzzShapingErrorKind::InvalidPlan:
            return "invalid_plan";
        case MultiRunCatalogHarfBuzzShapingErrorKind::InvalidBindingTable:
            return "invalid_binding_table";
        case MultiRunCatalogHarfBuzzShapingErrorKind::MissingFontRun:
            return "missing_font_run";
        case MultiRunCatalogHarfBuzzShapingErrorKind::FaceBindingNotFound:
            return "face_binding_not_found";
        case MultiRunCatalogHarfBuzzShapingErrorKind::GenerationMismatch:
            return "generation_mismatch";
        case MultiRunCatalogHarfBuzzShapingErrorKind::MetadataBudgetExceeded:
            return "metadata_budget_exceeded";
        case MultiRunCatalogHarfBuzzShapingErrorKind::SegmentShapingFailed:
            return "segment_shaping_failed";
        case MultiRunCatalogHarfBuzzShapingErrorKind::AggregateOverflow:
            return "aggregate_overflow";
    }
    return "invalid";
}

bool shape_multi_run_catalog_harfbuzz(
    const MultiRunCatalogHarfBuzzShapingRequest& request,
    MultiRunShapedText* output,
    MultiRunCatalogHarfBuzzShapingStats* stats,
    MultiRunCatalogHarfBuzzShapingError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);

    if (request.plan == nullptr) {
        return fail(
            MultiRunCatalogHarfBuzzShapingErrorKind::InvalidArgument,
            0U,
            0U,
            kInvalidFontFaceId,
            "multi-run shaping requires a shaping-run plan",
            error);
    }

    if (request.codepoints.empty()) {
        if (!request.grapheme_boundaries.empty() ||
            !request.plan->boundaries.empty()) {
            return fail(
                MultiRunCatalogHarfBuzzShapingErrorKind::InvalidPlan,
                0U,
                0U,
                kInvalidFontFaceId,
                "empty text requires empty grapheme and shaping-run inputs",
                error);
        }
        if (request.prepared_face_cache != nullptr) {
            stats->cache_before = request.prepared_face_cache->snapshot();
            stats->cache_after = stats->cache_before;
        }
        return true;
    }

    if (request.prepared_face_cache == nullptr ||
        request.grapheme_boundaries.size() < 2U ||
        request.grapheme_boundaries.front().codepoint_index != 0U ||
        request.grapheme_boundaries.back().codepoint_index !=
            request.codepoints.size()) {
        return fail(
            MultiRunCatalogHarfBuzzShapingErrorKind::InvalidArgument,
            0U,
            0U,
            kInvalidFontFaceId,
            "non-empty multi-run shaping requires a cache and complete grapheme domain",
            error);
    }

    FontGenerationFingerprint fingerprint{};
    std::uint64_t generation_id = 0U;
    if (!validate_binding_table(
            request.bindings,
            &fingerprint,
            &generation_id,
            error)) {
        return false;
    }

    const std::size_t cluster_count = request.grapheme_boundaries.size() - 1U;
    const auto& boundaries = request.plan->boundaries;
    if (boundaries.size() < 2U ||
        boundaries.front().cluster_index != 0U ||
        boundaries.back().cluster_index != cluster_count) {
        return fail(
            MultiRunCatalogHarfBuzzShapingErrorKind::InvalidPlan,
            0U,
            boundaries.empty() ? 0U : boundaries.front().cluster_index,
            kInvalidFontFaceId,
            "shaping-run plan must cover the complete grapheme domain",
            error);
    }

    const std::size_t run_count = boundaries.size() - 1U;
    constexpr std::size_t kBindingWordBits =
        std::numeric_limits<std::uint64_t>::digits;
    static_assert(kBindingWordBits == 64U);
    if (request.bindings.size() >
        std::numeric_limits<std::size_t>::max() -
            (kBindingWordBits - 1U)) {
        return fail(
            MultiRunCatalogHarfBuzzShapingErrorKind::MetadataBudgetExceeded,
            0U,
            0U,
            kInvalidFontFaceId,
            "binding-use bitmap size overflows",
            error);
    }
    const std::size_t binding_word_count =
        (request.bindings.size() + kBindingWordBits - 1U) /
        kBindingWordBits;
    std::pmr::vector<std::uint64_t> used_binding_words(
        output->metadata_resource());
    try {
        used_binding_words.resize(binding_word_count, std::uint64_t{0});
    } catch (const std::bad_alloc&) {
        return fail(
            MultiRunCatalogHarfBuzzShapingErrorKind::MetadataBudgetExceeded,
            0U,
            0U,
            kInvalidFontFaceId,
            "binding-use bitmap exceeds its hard budget",
            error);
    } catch (...) {
        return fail(
            MultiRunCatalogHarfBuzzShapingErrorKind::MetadataBudgetExceeded,
            0U,
            0U,
            kInvalidFontFaceId,
            "binding-use bitmap allocation failed",
            error);
    }
    std::uint64_t used_faces = 0U;

    for (std::size_t run_index = 0U; run_index < run_count; ++run_index) {
        const ShapingRunBoundary& current = boundaries[run_index];
        const ShapingRunBoundary& next = boundaries[run_index + 1U];
        if (current.cluster_index >= next.cluster_index ||
            next.cluster_index > cluster_count ||
            !valid_script(current.script) ||
            !valid_fallback_source(current.fallback_source) ||
            current.bidi_level > 126U ||
            (current.direction != ShapingDirection::LeftToRight &&
             current.direction != ShapingDirection::RightToLeft) ||
            ((current.bidi_level & 1U) == 0U) !=
                (current.direction == ShapingDirection::LeftToRight)) {
            return fail(
                MultiRunCatalogHarfBuzzShapingErrorKind::InvalidPlan,
                run_index,
                current.cluster_index,
                current.face_id,
                "shaping-run plan contains an invalid range or descriptor",
                error);
        }
        if (current.face_id == kInvalidFontFaceId ||
            current.fallback_source == FontFallbackSource::Missing) {
            return fail(
                MultiRunCatalogHarfBuzzShapingErrorKind::MissingFontRun,
                run_index,
                current.cluster_index,
                current.face_id,
                "missing-font runs require an explicit last-resort policy before shaping",
                error);
        }
        const CatalogFontFaceBinding* binding =
            find_binding(request.bindings, current.face_id);
        if (binding == nullptr) {
            return fail(
                MultiRunCatalogHarfBuzzShapingErrorKind::FaceBindingNotFound,
                run_index,
                current.cluster_index,
                current.face_id,
                "shaping-run face is absent from the immutable binding table",
                error);
        }
        const std::size_t binding_index = static_cast<std::size_t>(
            binding - request.bindings.data());
        const std::size_t word_index = binding_index / kBindingWordBits;
        const std::size_t bit_index = binding_index % kBindingWordBits;
        const std::uint64_t mask = std::uint64_t{1} << bit_index;
        if ((used_binding_words[word_index] & mask) == 0U) {
            used_binding_words[word_index] |= mask;
            ++used_faces;
        }
    }

    stats->cache_before = request.prepared_face_cache->snapshot();
    stats->generation_fingerprint = fingerprint;
    stats->generation_id = generation_id;
    stats->input_runs = run_count;
    stats->input_codepoints = request.codepoints.size();
    stats->input_clusters = cluster_count;
    stats->distinct_bound_faces = used_faces;
    release_vector(&used_binding_words);

    try {
        output->segments.reserve(run_count);
    } catch (const std::bad_alloc&) {
        stats->cache_after = request.prepared_face_cache->snapshot();
        return fail(
            MultiRunCatalogHarfBuzzShapingErrorKind::MetadataBudgetExceeded,
            0U,
            0U,
            kInvalidFontFaceId,
            "multi-run segment metadata exceeds its hard budget",
            error);
    } catch (...) {
        stats->cache_after = request.prepared_face_cache->snapshot();
        return fail(
            MultiRunCatalogHarfBuzzShapingErrorKind::MetadataBudgetExceeded,
            0U,
            0U,
            kInvalidFontFaceId,
            "multi-run segment metadata allocation failed",
            error);
    }

    for (std::size_t run_index = 0U; run_index < run_count; ++run_index) {
        const ShapingRunBoundary& current = boundaries[run_index];
        const ShapingRunBoundary& next = boundaries[run_index + 1U];
        const CatalogFontFaceBinding* binding =
            find_binding(request.bindings, current.face_id);

        try {
            output->segments.emplace_back(output->glyph_resource());
        } catch (...) {
            output->release();
            stats->cache_after = request.prepared_face_cache->snapshot();
            return fail(
                MultiRunCatalogHarfBuzzShapingErrorKind::MetadataBudgetExceeded,
                run_index,
                current.cluster_index,
                current.face_id,
                "multi-run segment construction failed",
                error);
        }

        MultiRunShapedSegment& segment = output->segments.back();
        segment.run = current;
        CachedCatalogHarfBuzzShapingRequest shaping_request;
        shaping_request.binding = binding;
        shaping_request.prepared_face_cache = request.prepared_face_cache;
        shaping_request.codepoints = request.codepoints;
        shaping_request.grapheme_boundaries = request.grapheme_boundaries;
        shaping_request.first_cluster = current.cluster_index;
        shaping_request.cluster_limit = next.cluster_index;
        shaping_request.script = current.script;
        shaping_request.direction = current.direction;
        shaping_request.language = request.language;
        shaping_request.features = request.features;
        shaping_request.variations = request.variations;
        shaping_request.x_scale = request.x_scale;
        shaping_request.y_scale = request.y_scale;
        shaping_request.beginning_of_text = current.cluster_index == 0U;
        shaping_request.end_of_text = next.cluster_index == cluster_count;
        shaping_request.produce_unsafe_to_concat =
            request.produce_unsafe_to_concat;

        CachedCatalogHarfBuzzShapingStats segment_stats;
        CachedCatalogHarfBuzzShapingError segment_error;
        if (!shape_cached_catalog_harfbuzz_segment(
                shaping_request,
                &segment.glyphs,
                &segment_stats,
                &segment_error)) {
            error->segment_error = std::move(segment_error);
            output->release();
            stats->cache_after = request.prepared_face_cache->snapshot();
            return fail(
                MultiRunCatalogHarfBuzzShapingErrorKind::SegmentShapingFailed,
                run_index,
                current.cluster_index,
                current.face_id,
                "one logical shaping run failed",
                error);
        }

        const HarfBuzzShapingStats& shaping = segment_stats.shaping.shaping;
        if (!add_segment_stats(shaping, stats)) {
            output->release();
            stats->cache_after = request.prepared_face_cache->snapshot();
            return fail(
                MultiRunCatalogHarfBuzzShapingErrorKind::AggregateOverflow,
                run_index,
                current.cluster_index,
                current.face_id,
                "multi-run shaping statistics overflowed",
                error);
        }
        stats->maximum_absolute_offset = std::max(
            stats->maximum_absolute_offset,
            shaping.maximum_absolute_offset);
        stats->maximum_run_glyphs = std::max(
            stats->maximum_run_glyphs,
            segment.glyphs.glyphs.size());
        ++stats->completed_runs;
        if (current.direction == ShapingDirection::LeftToRight) {
            ++stats->left_to_right_runs;
        } else {
            ++stats->right_to_left_runs;
        }
    }

    stats->cache_after = request.prepared_face_cache->snapshot();
    return true;
}

} // namespace zevryon::text
