#include "font_fallback.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::uint32_t kMaximumUnicodeCodePoint = 0x10ffffU;
constexpr std::size_t kScriptCount = static_cast<std::size_t>(ScriptId::Count);
constexpr std::uint64_t kScriptPenaltyUnit = 1000000U;

struct Selection {
    FontFaceId face_id{kInvalidFontFaceId};
    FontFallbackSource source{FontFallbackSource::Missing};
};

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

void clear_error(FontFallbackError* error) noexcept {
    if (error != nullptr) {
        error->kind = FontFallbackErrorKind::None;
        error->cluster_index = 0U;
        error->message.clear();
    }
}

bool fail(FontFallbackErrorKind kind,
          std::size_t cluster_index,
          const char* message,
          FontFallbackError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->cluster_index = cluster_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool valid_slant(FontSlant slant) noexcept {
    return slant == FontSlant::Upright ||
           slant == FontSlant::Italic ||
           slant == FontSlant::Oblique;
}

bool valid_script(ScriptId script) noexcept {
    return static_cast<std::size_t>(script) < kScriptCount;
}

bool neutral_script_id(ScriptId script) noexcept {
    return script == ScriptId::Zzzz ||
           script == ScriptId::Zinh ||
           script == ScriptId::Zyyy;
}

std::uint32_t range_plane_mask(FontCoverageRange range) noexcept {
    const std::uint32_t first_plane = range.first >> 16U;
    const std::uint32_t last_plane = range.last >> 16U;
    std::uint32_t mask = 0U;
    for (std::uint32_t plane = first_plane; plane <= last_plane; ++plane) {
        mask |= 1U << plane;
    }
    return mask;
}

std::uint32_t catalog_plane_mask(const FontCatalog& catalog) noexcept {
    std::uint32_t mask = 0U;
    for (const FontCoverageRange range : catalog.coverage_ranges) {
        mask |= range_plane_mask(range);
    }
    return mask;
}

std::uint32_t cluster_plane_mask(std::span<const DecodedCodePoint> codepoints,
                                 std::size_t first,
                                 std::size_t last) noexcept {
    std::uint32_t mask = 0U;
    for (std::size_t index = first; index < last; ++index) {
        const std::uint32_t value = codepoints[index].value;
        if (value > kMaximumUnicodeCodePoint) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        mask |= 1U << (value >> 16U);
    }
    return mask;
}

bool face_may_cover_planes(const FontCatalog& catalog,
                           FontFaceId face_id,
                           std::uint32_t required_planes) noexcept {
    if (face_id >= catalog.faces.size()) {
        return false;
    }
    const FontFaceRecord& face = catalog.faces[face_id];
    const std::size_t first = face.coverage_offset;
    const std::size_t count = face.coverage_count;
    if (first > catalog.coverage_ranges.size() ||
        count > catalog.coverage_ranges.size() - first) {
        return false;
    }
    std::uint32_t mask = 0U;
    for (std::size_t offset = 0U; offset < count; ++offset) {
        mask |= range_plane_mask(catalog.coverage_ranges[first + offset]);
        if ((mask & required_planes) == required_planes) {
            return true;
        }
    }
    return false;
}

bool validate_script_index(const FontCatalog& catalog,
                           FontFallbackError* error) noexcept {
    if (catalog.script_buckets.size() != kScriptCount ||
        catalog.script_face_ids.size() != catalog.faces.size()) {
        return fail(FontFallbackErrorKind::InvalidInput,
                    0U,
                    "font catalog script index dimensions are invalid",
                    error);
    }

    std::size_t expected_offset = 0U;
    for (std::size_t script_index = 0U;
         script_index < catalog.script_buckets.size();
         ++script_index) {
        const FontScriptBucket bucket = catalog.script_buckets[script_index];
        const std::size_t first = bucket.offset;
        const std::size_t count = bucket.count;
        if (first != expected_offset ||
            first > catalog.script_face_ids.size() ||
            count > catalog.script_face_ids.size() - first) {
            return fail(FontFallbackErrorKind::InvalidInput,
                        0U,
                        "font catalog script bucket offsets are invalid",
                        error);
        }

        FontFaceId previous = 0U;
        bool have_previous = false;
        for (std::size_t offset = 0U; offset < count; ++offset) {
            const FontFaceId face_id = catalog.script_face_ids[first + offset];
            if (face_id >= catalog.faces.size() ||
                (have_previous && face_id <= previous) ||
                static_cast<std::size_t>(catalog.faces[face_id].preferred_script) !=
                    script_index) {
                return fail(FontFallbackErrorKind::InvalidInput,
                            0U,
                            "font catalog script bucket membership is invalid",
                            error);
            }
            previous = face_id;
            have_previous = true;
        }
        expected_offset += count;
    }

    if (expected_offset != catalog.script_face_ids.size()) {
        return fail(FontFallbackErrorKind::InvalidInput,
                    0U,
                    "font catalog script index does not cover every face exactly once",
                    error);
    }
    return true;
}

bool validate_catalog(const FontCatalog& catalog,
                      FontFallbackError* error) noexcept {
    if (catalog.faces.size() > static_cast<std::size_t>(
                                   std::numeric_limits<std::uint32_t>::max())) {
        return fail(FontFallbackErrorKind::IndexOverflow,
                    0U,
                    "font catalog exceeds the 32-bit face identifier contract",
                    error);
    }

    std::uint64_t previous_key = 0U;
    bool have_previous_key = false;
    for (std::size_t face_index = 0U;
         face_index < catalog.faces.size();
         ++face_index) {
        const FontFaceRecord& face = catalog.faces[face_index];
        if (face.stable_key == 0U ||
            (have_previous_key && face.stable_key <= previous_key) ||
            face.weight == 0U || face.weight > 1000U ||
            face.width == 0U || face.width > 9U ||
            !valid_slant(face.slant) || !valid_script(face.preferred_script)) {
            return fail(FontFallbackErrorKind::InvalidInput,
                        0U,
                        "font catalog face metadata is invalid",
                        error);
        }
        previous_key = face.stable_key;
        have_previous_key = true;

        const std::size_t first = face.coverage_offset;
        const std::size_t count = face.coverage_count;
        if (count == 0U || first > catalog.coverage_ranges.size() ||
            count > catalog.coverage_ranges.size() - first) {
            return fail(FontFallbackErrorKind::InvalidInput,
                        0U,
                        "font catalog coverage offsets are invalid",
                        error);
        }
        std::uint32_t previous_last = 0U;
        bool have_previous_range = false;
        for (std::size_t offset = 0U; offset < count; ++offset) {
            const FontCoverageRange range = catalog.coverage_ranges[first + offset];
            if (range.first > range.last || range.last > kMaximumUnicodeCodePoint ||
                (have_previous_range && range.first <= previous_last)) {
                return fail(FontFallbackErrorKind::InvalidInput,
                            0U,
                            "font catalog coverage ranges are invalid",
                            error);
            }
            previous_last = range.last;
            have_previous_range = true;
        }
    }
    return validate_script_index(catalog, error);
}

bool validate_inputs(std::span<const DecodedCodePoint> codepoints,
                     std::span<const GraphemeBoundary> graphemes,
                     std::span<const ScriptRunBoundary> script_runs,
                     const FontCatalog& catalog,
                     const FontFallbackRequest& request,
                     FontFallbackError* error) noexcept {
    if (!validate_catalog(catalog, error)) {
        return false;
    }
    if (request.style.weight == 0U || request.style.weight > 1000U ||
        request.style.width == 0U || request.style.width > 9U ||
        !valid_slant(request.style.slant)) {
        return fail(FontFallbackErrorKind::InvalidInput,
                    0U,
                    "font style request is invalid",
                    error);
    }
    if (request.primary_face != kInvalidFontFaceId &&
        request.primary_face >= catalog.faces.size()) {
        return fail(FontFallbackErrorKind::InvalidInput,
                    0U,
                    "primary font face is outside the catalog",
                    error);
    }
    for (const FontFaceId face_id : request.preferred_faces) {
        if (face_id >= catalog.faces.size()) {
            return fail(FontFallbackErrorKind::InvalidInput,
                        0U,
                        "preferred font face is outside the catalog",
                        error);
        }
    }

    if (codepoints.empty()) {
        if (!graphemes.empty() || !script_runs.empty()) {
            return fail(FontFallbackErrorKind::InvalidInput,
                        0U,
                        "empty text must not carry grapheme or script boundaries",
                        error);
        }
        return true;
    }
    if (codepoints.size() > static_cast<std::size_t>(
                                std::numeric_limits<std::uint32_t>::max()) ||
        graphemes.size() < 2U || script_runs.size() < 2U) {
        return fail(FontFallbackErrorKind::IndexOverflow,
                    0U,
                    "text boundaries exceed or violate the 32-bit fallback contract",
                    error);
    }

    if (graphemes.front().codepoint_index != 0U ||
        graphemes.back().codepoint_index != codepoints.size()) {
        return fail(FontFallbackErrorKind::InvalidInput,
                    0U,
                    "grapheme boundaries do not cover the complete codepoint stream",
                    error);
    }
    for (std::size_t index = 1U; index < graphemes.size(); ++index) {
        if (graphemes[index - 1U].codepoint_index >=
                graphemes[index].codepoint_index ||
            graphemes[index - 1U].source_offset > graphemes[index].source_offset) {
            return fail(FontFallbackErrorKind::InvalidInput,
                        index - 1U,
                        "grapheme boundaries must be strictly increasing",
                        error);
        }
    }

    const std::size_t cluster_count = graphemes.size() - 1U;
    if (cluster_count > static_cast<std::size_t>(
                            std::numeric_limits<std::uint32_t>::max()) ||
        script_runs.front().cluster_index != 0U ||
        script_runs.back().cluster_index != cluster_count) {
        return fail(FontFallbackErrorKind::InvalidInput,
                    0U,
                    "script runs do not cover the complete grapheme stream",
                    error);
    }
    for (std::size_t index = 1U; index < script_runs.size(); ++index) {
        if (script_runs[index - 1U].cluster_index >=
                script_runs[index].cluster_index ||
            script_runs[index].cluster_index > cluster_count ||
            !valid_script(script_runs[index - 1U].script)) {
            return fail(FontFallbackErrorKind::InvalidInput,
                        script_runs[index - 1U].cluster_index,
                        "script run boundaries must be strictly increasing and valid",
                        error);
        }
    }
    if (!valid_script(script_runs.back().script)) {
        return fail(FontFallbackErrorKind::InvalidInput,
                    cluster_count,
                    "script run sentinel carries an invalid script",
                    error);
    }
    return true;
}

bool covers_cluster(const FontCatalog& catalog,
                    FontFaceId face_id,
                    std::span<const DecodedCodePoint> codepoints,
                    std::size_t first,
                    std::size_t last,
                    std::uint64_t* coverage_checks) noexcept {
    if (coverage_checks != nullptr) {
        ++(*coverage_checks);
    }
    for (std::size_t index = first; index < last; ++index) {
        if (!font_face_covers(catalog, face_id, codepoints[index].value)) {
            return false;
        }
    }
    return true;
}

std::uint64_t unsigned_difference(std::uint32_t left,
                                  std::uint32_t right) noexcept {
    return left >= right
        ? static_cast<std::uint64_t>(left - right)
        : static_cast<std::uint64_t>(right - left);
}

std::uint64_t script_penalty(ScriptId face_script,
                             ScriptId text_script) noexcept {
    if (neutral_script_id(text_script)) {
        return neutral_script_id(face_script) ? 0U : 1U;
    }
    if (face_script == text_script) {
        return 0U;
    }
    return neutral_script_id(face_script) ? 1U : 2U;
}

std::uint64_t style_score(const FontFaceRecord& face,
                          ScriptId script,
                          const FontStyleRequest& request) noexcept {
    std::uint64_t slant_penalty = 0U;
    if (face.slant != request.slant) {
        const bool related_slants =
            (face.slant == FontSlant::Italic &&
             request.slant == FontSlant::Oblique) ||
            (face.slant == FontSlant::Oblique &&
             request.slant == FontSlant::Italic);
        slant_penalty = related_slants ? 50U : 200U;
    }

    return script_penalty(face.preferred_script, script) * kScriptPenaltyUnit +
           slant_penalty * 10000U +
           unsigned_difference(face.width, request.width) * 1000U +
           unsigned_difference(face.weight, request.weight);
}

FontFallbackSource general_source(const FontFaceRecord& face,
                                  ScriptId script) noexcept {
    if (!neutral_script_id(script) && face.preferred_script == script) {
        return FontFallbackSource::ScriptMatch;
    }
    if (neutral_script_id(script) ||
        neutral_script_id(face.preferred_script)) {
        return FontFallbackSource::NeutralScript;
    }
    return FontFallbackSource::CrossScript;
}

bool candidate_covers(const FontCatalog& catalog,
                      FontFaceId face_id,
                      std::uint32_t required_planes,
                      std::span<const DecodedCodePoint> codepoints,
                      std::size_t first,
                      std::size_t last,
                      std::uint64_t* coverage_checks) noexcept {
    return face_may_cover_planes(catalog, face_id, required_planes) &&
           covers_cluster(catalog,
                          face_id,
                          codepoints,
                          first,
                          last,
                          coverage_checks);
}

void scan_bucket(ScriptId bucket_script,
                 ScriptId text_script,
                 std::span<const DecodedCodePoint> codepoints,
                 std::size_t first,
                 std::size_t last,
                 const FontCatalog& catalog,
                 const FontStyleRequest& request,
                 std::uint32_t required_planes,
                 std::uint64_t* coverage_checks,
                 std::array<bool, kScriptCount>* visited,
                 FontFaceId* best_face,
                 std::uint64_t* best_score,
                 std::uint64_t* best_key) noexcept {
    const std::size_t bucket_index = static_cast<std::size_t>(bucket_script);
    if (bucket_index >= visited->size() || (*visited)[bucket_index]) {
        return;
    }
    (*visited)[bucket_index] = true;

    const std::uint64_t lower_bound =
        script_penalty(bucket_script, text_script) * kScriptPenaltyUnit;
    if (*best_score < lower_bound) {
        return;
    }

    for (const FontFaceId face_id : font_faces_for_script(catalog, bucket_script)) {
        const FontFaceRecord& face = catalog.faces[face_id];
        const std::uint64_t score = style_score(face, text_script, request);
        if (score > *best_score ||
            (score == *best_score && face.stable_key >= *best_key)) {
            continue;
        }
        if (!candidate_covers(catalog,
                              face_id,
                              required_planes,
                              codepoints,
                              first,
                              last,
                              coverage_checks)) {
            continue;
        }
        *best_face = face_id;
        *best_score = score;
        *best_key = face.stable_key;
    }
}

Selection finish_selection(FontFaceId best_face,
                           ScriptId script,
                           const FontCatalog& catalog) noexcept {
    if (best_face == kInvalidFontFaceId) {
        return {};
    }
    return {best_face, general_source(catalog.faces[best_face], script)};
}

Selection select_face(std::span<const DecodedCodePoint> codepoints,
                      std::size_t first,
                      std::size_t last,
                      ScriptId script,
                      const FontCatalog& catalog,
                      const FontFallbackRequest& request,
                      std::uint32_t all_catalog_planes,
                      std::uint64_t* coverage_checks) noexcept {
    const std::uint32_t required_planes =
        cluster_plane_mask(codepoints, first, last);
    if ((all_catalog_planes & required_planes) != required_planes) {
        return {};
    }

    if (request.primary_face != kInvalidFontFaceId &&
        candidate_covers(catalog,
                         request.primary_face,
                         required_planes,
                         codepoints,
                         first,
                         last,
                         coverage_checks)) {
        return {request.primary_face, FontFallbackSource::Primary};
    }

    for (const FontFaceId face_id : request.preferred_faces) {
        if (candidate_covers(catalog,
                             face_id,
                             required_planes,
                             codepoints,
                             first,
                             last,
                             coverage_checks)) {
            return {face_id, FontFallbackSource::PreferredFamily};
        }
    }

    FontFaceId best_face = kInvalidFontFaceId;
    std::uint64_t best_score = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t best_key = std::numeric_limits<std::uint64_t>::max();
    std::array<bool, kScriptCount> visited{};

    constexpr std::array<ScriptId, 3> neutral_buckets{{
        ScriptId::Zzzz,
        ScriptId::Zinh,
        ScriptId::Zyyy,
    }};

    if (!neutral_script_id(script)) {
        scan_bucket(script,
                    script,
                    codepoints,
                    first,
                    last,
                    catalog,
                    request.style,
                    required_planes,
                    coverage_checks,
                    &visited,
                    &best_face,
                    &best_score,
                    &best_key);
        if (best_score < kScriptPenaltyUnit) {
            return finish_selection(best_face, script, catalog);
        }
    }

    for (const ScriptId neutral : neutral_buckets) {
        scan_bucket(neutral,
                    script,
                    codepoints,
                    first,
                    last,
                    catalog,
                    request.style,
                    required_planes,
                    coverage_checks,
                    &visited,
                    &best_face,
                    &best_score,
                    &best_key);
    }

    const std::uint64_t cross_script_lower_bound =
        neutral_script_id(script) ? kScriptPenaltyUnit
                                  : 2U * kScriptPenaltyUnit;
    if (best_score < cross_script_lower_bound) {
        return finish_selection(best_face, script, catalog);
    }

    for (std::size_t bucket = 0U; bucket < kScriptCount; ++bucket) {
        scan_bucket(static_cast<ScriptId>(bucket),
                    script,
                    codepoints,
                    first,
                    last,
                    catalog,
                    request.style,
                    required_planes,
                    coverage_checks,
                    &visited,
                    &best_face,
                    &best_score,
                    &best_key);
    }

    return finish_selection(best_face, script, catalog);
}

void record_source(FontFallbackSource source,
                   FontFallbackStats* stats) noexcept {
    switch (source) {
        case FontFallbackSource::Primary:
            ++stats->primary_clusters;
            return;
        case FontFallbackSource::PreferredFamily:
            ++stats->preferred_family_clusters;
            return;
        case FontFallbackSource::ScriptMatch:
            ++stats->script_match_clusters;
            return;
        case FontFallbackSource::NeutralScript:
            ++stats->neutral_script_clusters;
            return;
        case FontFallbackSource::CrossScript:
            ++stats->cross_script_clusters;
            return;
        case FontFallbackSource::Missing:
            ++stats->missing_clusters;
            return;
    }
}

ScriptId script_for_cluster(std::span<const ScriptRunBoundary> script_runs,
                            std::size_t cluster_index,
                            std::size_t* run_index) noexcept {
    while (*run_index + 1U < script_runs.size() - 1U &&
           cluster_index >= script_runs[*run_index + 1U].cluster_index) {
        ++(*run_index);
    }
    return script_runs[*run_index].script;
}

} // namespace

FontFallbackPlan::FontFallbackPlan(std::pmr::memory_resource* resource)
    : boundaries(resource) {}

std::pmr::memory_resource* FontFallbackPlan::resource() const noexcept {
    return boundaries.get_allocator().resource();
}

void FontFallbackPlan::release() noexcept {
    release_vector(&boundaries);
}

const char* font_fallback_error_kind_name(FontFallbackErrorKind kind) noexcept {
    switch (kind) {
        case FontFallbackErrorKind::None:
            return "none";
        case FontFallbackErrorKind::InvalidInput:
            return "invalid_input";
        case FontFallbackErrorKind::IndexOverflow:
            return "index_overflow";
        case FontFallbackErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

const char* font_fallback_source_name(FontFallbackSource source) noexcept {
    switch (source) {
        case FontFallbackSource::Primary:
            return "primary";
        case FontFallbackSource::PreferredFamily:
            return "preferred_family";
        case FontFallbackSource::ScriptMatch:
            return "script_match";
        case FontFallbackSource::NeutralScript:
            return "neutral_script";
        case FontFallbackSource::CrossScript:
            return "cross_script";
        case FontFallbackSource::Missing:
            return "missing";
    }
    return "invalid";
}

bool build_font_fallback_plan(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const GraphemeBoundary> grapheme_boundaries,
    std::span<const ScriptRunBoundary> script_run_boundaries,
    const FontCatalog& catalog,
    const FontFallbackRequest& request,
    FontFallbackPlan* output,
    FontFallbackStats* stats,
    FontFallbackError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    output->release();
    *stats = {};

    if (!validate_inputs(codepoints,
                         grapheme_boundaries,
                         script_run_boundaries,
                         catalog,
                         request,
                         error)) {
        return false;
    }
    if (codepoints.empty()) {
        return true;
    }

    const std::size_t cluster_count = grapheme_boundaries.size() - 1U;
    const std::uint32_t all_catalog_planes = catalog_plane_mask(catalog);
    FontFallbackStats working_stats;
    working_stats.input_codepoints = static_cast<std::uint64_t>(codepoints.size());
    working_stats.input_clusters = static_cast<std::uint64_t>(cluster_count);
    working_stats.catalog_faces = static_cast<std::uint64_t>(catalog.faces.size());

    std::size_t output_runs = 0U;
    Selection previous;
    bool have_previous = false;
    std::size_t script_run_index = 0U;
    for (std::size_t cluster = 0U; cluster < cluster_count; ++cluster) {
        const std::size_t first = grapheme_boundaries[cluster].codepoint_index;
        const std::size_t last = grapheme_boundaries[cluster + 1U].codepoint_index;
        working_stats.maximum_cluster_codepoints = std::max(
            working_stats.maximum_cluster_codepoints,
            static_cast<std::uint64_t>(last - first));
        const ScriptId script = script_for_cluster(script_run_boundaries,
                                                   cluster,
                                                   &script_run_index);
        const Selection selection = select_face(codepoints,
                                                first,
                                                last,
                                                script,
                                                catalog,
                                                request,
                                                all_catalog_planes,
                                                &working_stats.coverage_checks);
        record_source(selection.source, &working_stats);
        if (!have_previous || selection.face_id != previous.face_id ||
            selection.source != previous.source) {
            ++output_runs;
            previous = selection;
            have_previous = true;
        }
    }

    if (output_runs == std::numeric_limits<std::size_t>::max() ||
        output_runs + 1U > static_cast<std::size_t>(
                               std::numeric_limits<std::uint32_t>::max())) {
        return fail(FontFallbackErrorKind::IndexOverflow,
                    0U,
                    "font fallback run count exceeds the 32-bit boundary contract",
                    error);
    }

    try {
        FontFallbackPlan working(output->resource());
        working.boundaries.reserve(output_runs + 1U);
        have_previous = false;
        script_run_index = 0U;
        for (std::size_t cluster = 0U; cluster < cluster_count; ++cluster) {
            const std::size_t first = grapheme_boundaries[cluster].codepoint_index;
            const std::size_t last = grapheme_boundaries[cluster + 1U].codepoint_index;
            const ScriptId script = script_for_cluster(script_run_boundaries,
                                                       cluster,
                                                       &script_run_index);
            const Selection selection = select_face(codepoints,
                                                    first,
                                                    last,
                                                    script,
                                                    catalog,
                                                    request,
                                                    all_catalog_planes,
                                                    nullptr);
            if (!have_previous || selection.face_id != previous.face_id ||
                selection.source != previous.source) {
                working.boundaries.push_back(FontFallbackBoundary{
                    static_cast<std::uint32_t>(cluster),
                    selection.face_id,
                    selection.source,
                    0U,
                    0U,
                    0U});
                previous = selection;
                have_previous = true;
            }
        }
        working.boundaries.push_back(FontFallbackBoundary{
            static_cast<std::uint32_t>(cluster_count),
            kInvalidFontFaceId,
            FontFallbackSource::Missing,
            0U,
            0U,
            0U});
        working_stats.output_runs = static_cast<std::uint64_t>(output_runs);
        output->boundaries.swap(working.boundaries);
        *stats = working_stats;
        return true;
    } catch (const std::bad_alloc&) {
        return fail(FontFallbackErrorKind::OutputBudgetExceeded,
                    0U,
                    "font fallback output exceeded its resource budget",
                    error);
    } catch (...) {
        return fail(FontFallbackErrorKind::OutputBudgetExceeded,
                    0U,
                    "font fallback allocation failed",
                    error);
    }
}

} // namespace zevryon::text
