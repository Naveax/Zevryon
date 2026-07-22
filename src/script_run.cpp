#include "script_run.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::size_t kScriptCount = static_cast<std::size_t>(ScriptId::Count);
constexpr std::size_t kMaskWords = (kScriptCount + 63U) / 64U;

struct ScriptMask {
    std::array<std::uint64_t, kMaskWords> words{};

    void set(ScriptId script) noexcept {
        const std::size_t index = static_cast<std::size_t>(script);
        if (index < kScriptCount) {
            words[index / 64U] |= std::uint64_t{1} << (index % 64U);
        }
    }

    bool contains(ScriptId script) const noexcept {
        const std::size_t index = static_cast<std::size_t>(script);
        return index < kScriptCount &&
               (words[index / 64U] & (std::uint64_t{1} << (index % 64U))) != 0U;
    }

    bool any() const noexcept {
        for (const std::uint64_t word : words) {
            if (word != 0U) {
                return true;
            }
        }
        return false;
    }

    void intersect(const ScriptMask& other) noexcept {
        for (std::size_t index = 0U; index < words.size(); ++index) {
            words[index] &= other.words[index];
        }
    }

    ScriptId first() const noexcept {
        for (std::size_t word_index = 0U; word_index < words.size(); ++word_index) {
            const std::uint64_t word = words[word_index];
            if (word == 0U) {
                continue;
            }
            for (std::size_t bit = 0U; bit < 64U; ++bit) {
                if ((word & (std::uint64_t{1} << bit)) != 0U) {
                    const std::size_t index = word_index * 64U + bit;
                    return index < kScriptCount
                        ? static_cast<ScriptId>(index)
                        : ScriptId::Zzzz;
                }
            }
        }
        return ScriptId::Zzzz;
    }
};

struct ClusterScripts {
    ScriptMask candidates;
    ScriptId preferred{ScriptId::Zzzz};
    bool strong{false};
};

void clear_error(ScriptRunError* error) noexcept {
    if (error != nullptr) {
        error->kind = ScriptRunErrorKind::None;
        error->cluster_index = 0U;
        error->message.clear();
    }
}

bool fail(
    ScriptRunErrorKind kind,
    std::size_t cluster_index,
    const char* message,
    ScriptRunError* error) noexcept {
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

ScriptMask strong_mask(const ScriptSetView& scripts) noexcept {
    ScriptMask mask;
    for (std::size_t index = 0U; index < scripts.size(); ++index) {
        const ScriptId script = scripts[index];
        if (!is_neutral_script(script)) {
            mask.set(script);
        }
    }
    return mask;
}

ScriptId preferred_script(
    const ScriptSetView& scripts,
    const ScriptMask& mask) noexcept {
    const ScriptId primary = scripts.primary();
    return !is_neutral_script(primary) && mask.contains(primary)
        ? primary
        : mask.first();
}

ClusterScripts cluster_scripts(
    std::span<const DecodedCodePoint> codepoints,
    std::size_t first,
    std::size_t end,
    ScriptRunStats* stats) noexcept {
    ClusterScripts result;
    for (std::size_t index = first; index < end; ++index) {
        const ScriptSetView scripts = script_extensions(codepoints[index].value);
        if (scripts.has_explicit_extensions()) {
            ++stats->explicit_extension_lookups;
        }
        const ScriptMask current = strong_mask(scripts);
        if (!current.any()) {
            continue;
        }
        const ScriptId current_preferred = preferred_script(scripts, current);
        if (!result.strong) {
            result.candidates = current;
            result.preferred = current_preferred;
            result.strong = true;
            continue;
        }

        ScriptMask intersection = result.candidates;
        intersection.intersect(current);
        if (!intersection.any()) {
            // Grapheme atomicity takes precedence. Keep the first strong scalar's
            // candidate set and record the incompatible internal property mix.
            ++stats->internal_cluster_conflicts;
            continue;
        }
        result.candidates = intersection;
        if (!result.candidates.contains(result.preferred)) {
            result.preferred = result.candidates.contains(current_preferred)
                ? current_preferred
                : result.candidates.first();
        }
    }
    return result;
}

ScriptId choose_script(
    const ScriptMask& candidates,
    ScriptId preferred) noexcept {
    return candidates.contains(preferred) ? preferred : candidates.first();
}

bool push_boundary(
    std::uint64_t source_offset,
    std::size_t cluster_index,
    ScriptId script,
    std::pmr::vector<ScriptRunBoundary>* boundaries,
    ScriptRunError* error) noexcept {
    if (cluster_index > static_cast<std::size_t>(
                            std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            ScriptRunErrorKind::IndexOverflow,
            cluster_index,
            "script-run cluster index exceeds 32-bit storage",
            error);
    }
    try {
        boundaries->push_back({
            source_offset,
            static_cast<std::uint32_t>(cluster_index),
            script,
            0U,
        });
    } catch (const std::bad_alloc&) {
        return fail(
            ScriptRunErrorKind::OutputBudgetExceeded,
            cluster_index,
            "script-run output exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            ScriptRunErrorKind::OutputBudgetExceeded,
            cluster_index,
            "script-run output allocation failed",
            error);
    }
    return true;
}

bool validate_input(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const GraphemeBoundary> grapheme_boundaries,
    ScriptRunError* error) noexcept {
    if (codepoints.empty()) {
        return grapheme_boundaries.empty() || fail(
            ScriptRunErrorKind::InvalidInput,
            0U,
            "empty codepoint input requires an empty grapheme boundary list",
            error);
    }
    if (grapheme_boundaries.size() < 2U ||
        grapheme_boundaries.front().codepoint_index != 0U ||
        grapheme_boundaries.front().source_offset != codepoints.front().source_start ||
        grapheme_boundaries.back().codepoint_index != codepoints.size() ||
        grapheme_boundaries.back().source_offset != codepoints.back().source_end()) {
        return fail(
            ScriptRunErrorKind::InvalidInput,
            0U,
            "grapheme boundary sentinels do not match decoded input",
            error);
    }

    for (std::size_t index = 0U; index < codepoints.size(); ++index) {
        const DecodedCodePoint& codepoint = codepoints[index];
        if (codepoint.source_length == 0U || codepoint.source_length > 4U ||
            codepoint.value > 0x10ffffU ||
            (codepoint.value >= 0xd800U && codepoint.value <= 0xdfffU) ||
            (index != 0U &&
             codepoint.source_start != codepoints[index - 1U].source_end())) {
            return fail(
                ScriptRunErrorKind::InvalidInput,
                index,
                "decoded codepoint input is not a contiguous Unicode scalar stream",
                error);
        }
    }

    for (std::size_t index = 0U; index + 1U < grapheme_boundaries.size(); ++index) {
        const GraphemeBoundary& current = grapheme_boundaries[index];
        const GraphemeBoundary& next = grapheme_boundaries[index + 1U];
        if (current.codepoint_index >= next.codepoint_index ||
            current.source_offset >= next.source_offset ||
            next.codepoint_index > codepoints.size()) {
            return fail(
                ScriptRunErrorKind::InvalidInput,
                index,
                "grapheme boundaries are not strictly increasing",
                error);
        }
        if (current.source_offset !=
            codepoints[current.codepoint_index].source_start) {
            return fail(
                ScriptRunErrorKind::InvalidInput,
                index,
                "grapheme boundary source offset does not match its codepoint",
                error);
        }
        if (next.codepoint_index < codepoints.size() &&
            next.source_offset != codepoints[next.codepoint_index].source_start) {
            return fail(
                ScriptRunErrorKind::InvalidInput,
                index + 1U,
                "grapheme boundary source offset does not match its codepoint",
                error);
        }
    }
    return true;
}

} // namespace

const char* script_run_error_kind_name(ScriptRunErrorKind kind) noexcept {
    switch (kind) {
        case ScriptRunErrorKind::None:
            return "none";
        case ScriptRunErrorKind::InvalidInput:
            return "invalid_input";
        case ScriptRunErrorKind::IndexOverflow:
            return "index_overflow";
        case ScriptRunErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

bool resolve_script_runs(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const GraphemeBoundary> grapheme_boundaries,
    std::pmr::vector<ScriptRunBoundary>* run_boundaries,
    ScriptRunStats* stats,
    ScriptRunError* error) noexcept {
    if (run_boundaries == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    run_boundaries->clear();
    *stats = {};
    stats->input_codepoints = static_cast<std::uint64_t>(codepoints.size());
    if (!validate_input(codepoints, grapheme_boundaries, error)) {
        return false;
    }
    if (codepoints.empty()) {
        return true;
    }

    const std::size_t cluster_count = grapheme_boundaries.size() - 1U;
    stats->input_clusters = static_cast<std::uint64_t>(cluster_count);
    std::size_t run_start = 0U;
    bool have_strong_run = false;
    ScriptMask active_candidates;
    ScriptId active_preferred = ScriptId::Zzzz;

    for (std::size_t cluster = 0U; cluster < cluster_count; ++cluster) {
        const std::size_t first = grapheme_boundaries[cluster].codepoint_index;
        const std::size_t end = grapheme_boundaries[cluster + 1U].codepoint_index;
        const ClusterScripts current =
            cluster_scripts(codepoints, first, end, stats);
        if (!current.strong) {
            ++stats->neutral_clusters;
            continue;
        }
        if (!have_strong_run) {
            active_candidates = current.candidates;
            active_preferred = current.preferred;
            have_strong_run = true;
            continue;
        }

        ScriptMask intersection = active_candidates;
        intersection.intersect(current.candidates);
        if (intersection.any()) {
            active_candidates = intersection;
            if (!active_candidates.contains(active_preferred)) {
                active_preferred = active_candidates.contains(current.preferred)
                    ? current.preferred
                    : active_candidates.first();
            }
            continue;
        }

        if (!push_boundary(
                grapheme_boundaries[run_start].source_offset,
                run_start,
                choose_script(active_candidates, active_preferred),
                run_boundaries,
                error)) {
            return false;
        }
        ++stats->output_runs;
        ++stats->run_splits;
        stats->maximum_run_clusters = std::max(
            stats->maximum_run_clusters,
            static_cast<std::uint64_t>(cluster - run_start));
        run_start = cluster;
        active_candidates = current.candidates;
        active_preferred = current.preferred;
    }

    const ScriptId final_script = have_strong_run
        ? choose_script(active_candidates, active_preferred)
        : ScriptId::Zyyy;
    if (!push_boundary(
            grapheme_boundaries[run_start].source_offset,
            run_start,
            final_script,
            run_boundaries,
            error)) {
        return false;
    }
    ++stats->output_runs;
    stats->maximum_run_clusters = std::max(
        stats->maximum_run_clusters,
        static_cast<std::uint64_t>(cluster_count - run_start));
    if (!push_boundary(
            grapheme_boundaries.back().source_offset,
            cluster_count,
            ScriptId::Zzzz,
            run_boundaries,
            error)) {
        return false;
    }
    return true;
}

} // namespace zevryon::text
