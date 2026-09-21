#include "css_offscreen_style_selection_v1.hpp"

#include <new>
#include <utility>

namespace zevryon::style {
namespace {

struct EligibleStyleV1 {
    std::uint32_t terminal_node{0U};
    std::uint32_t distance_css_px{0U};
};

bool set_error(
    CssOffscreenStyleSelectionErrorV1* error,
    CssOffscreenStyleSelectionErrorKindV1 kind,
    std::size_t candidate_index,
    const char* message) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->candidate_index = candidate_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool consume_work(
    CssOffscreenStyleSelectionConfigV1 config,
    CssOffscreenStyleSelectionStatsV1* stats,
    std::uint64_t units) noexcept {
    if (stats == nullptr ||
        stats->work_units > config.maximum_work_units ||
        units > config.maximum_work_units - stats->work_units) {
        return false;
    }
    stats->work_units += units;
    return true;
}

bool before(
    const EligibleStyleV1& left,
    const EligibleStyleV1& right) noexcept {
    if (left.distance_css_px != right.distance_css_px) {
        return left.distance_css_px < right.distance_css_px;
    }
    return left.terminal_node < right.terminal_node;
}

} // namespace

CssOffscreenStyleSelectionV1::CssOffscreenStyleSelectionV1(
    std::pmr::memory_resource* memory)
    : terminal_nodes(memory) {}

std::pmr::memory_resource*
CssOffscreenStyleSelectionV1::resource() const noexcept {
    return terminal_nodes.get_allocator().resource();
}

void CssOffscreenStyleSelectionV1::release() noexcept {
    std::pmr::vector<std::uint32_t> empty(resource());
    terminal_nodes.swap(empty);
}

bool CssOffscreenStyleSelectionConfigV1::valid() const noexcept {
    return maximum_candidates > 0U &&
        maximum_candidates <= kMaximumCandidatesLimit &&
        maximum_selected_styles > 0U &&
        maximum_selected_styles <= kMaximumSelectedStylesLimit &&
        maximum_selected_styles <= maximum_candidates &&
        lookahead_css_px <= kMaximumLookaheadCssPxLimit &&
        maximum_work_units > 0U &&
        maximum_work_units <= kMaximumWorkUnitsLimit;
}

const char* css_offscreen_style_selection_error_kind_name_v1(
    CssOffscreenStyleSelectionErrorKindV1 kind) noexcept {
    switch (kind) {
    case CssOffscreenStyleSelectionErrorKindV1::None:
        return "none";
    case CssOffscreenStyleSelectionErrorKindV1::InvalidConfiguration:
        return "invalid-configuration";
    case CssOffscreenStyleSelectionErrorKindV1::CandidateLimitExceeded:
        return "candidate-limit-exceeded";
    case CssOffscreenStyleSelectionErrorKindV1::WorkBudgetExceeded:
        return "work-budget-exceeded";
    case CssOffscreenStyleSelectionErrorKindV1::AllocationFailure:
        return "allocation-failure";
    }
    return "unknown";
}

bool select_css_offscreen_style_nodes_v1(
    std::span<const CssOffscreenStyleCandidateV1> candidates,
    CssOffscreenStyleSelectionConfigV1 config,
    CssOffscreenStyleSelectionV1* output,
    CssOffscreenStyleSelectionStatsV1* stats,
    CssOffscreenStyleSelectionErrorV1* error) noexcept {
    if (stats != nullptr) {
        *stats = CssOffscreenStyleSelectionStatsV1{};
    }
    if (error != nullptr) {
        error->kind = CssOffscreenStyleSelectionErrorKindV1::None;
        error->candidate_index = 0U;
        error->message.clear();
    }
    if (output == nullptr || stats == nullptr || error == nullptr ||
        output->resource() == nullptr || !config.valid()) {
        return set_error(
            error,
            CssOffscreenStyleSelectionErrorKindV1::InvalidConfiguration,
            0U,
            "CSS offscreen style selection requires output, stats, error and valid configuration");
    }
    if (candidates.size() > config.maximum_candidates) {
        return set_error(
            error,
            CssOffscreenStyleSelectionErrorKindV1::CandidateLimitExceeded,
            config.maximum_candidates,
            "CSS offscreen style candidate count exceeds configured limit");
    }

    CssOffscreenStyleSelectionV1 candidate_output(output->resource());
    std::pmr::vector<EligibleStyleV1> eligible(output->resource());

    try {
        eligible.reserve(candidates.size());
        for (std::size_t index = 0U; index < candidates.size(); ++index) {
            const CssOffscreenStyleCandidateV1& candidate =
                candidates[index];
            ++stats->candidates_considered;
            if (!consume_work(config, stats, 1U)) {
                return set_error(
                    error,
                    CssOffscreenStyleSelectionErrorKindV1::WorkBudgetExceeded,
                    index,
                    "CSS offscreen style candidate scan exceeded work budget");
            }
            if (candidate.intersects_viewport) {
                ++stats->visible_candidates_skipped;
                continue;
            }
            if (candidate.distance_css_px > config.lookahead_css_px) {
                ++stats->outside_lookahead_skipped;
                continue;
            }

            bool duplicate = false;
            for (EligibleStyleV1& existing : eligible) {
                if (!consume_work(config, stats, 1U)) {
                    return set_error(
                        error,
                        CssOffscreenStyleSelectionErrorKindV1::WorkBudgetExceeded,
                        index,
                        "CSS offscreen style deduplication exceeded work budget");
                }
                if (existing.terminal_node != candidate.terminal_node) {
                    continue;
                }
                if (candidate.distance_css_px < existing.distance_css_px) {
                    existing.distance_css_px =
                        candidate.distance_css_px;
                }
                ++stats->duplicate_terminal_candidates;
                duplicate = true;
                break;
            }
            if (!duplicate) {
                eligible.push_back(
                    EligibleStyleV1{
                        candidate.terminal_node,
                        candidate.distance_css_px});
            }
        }

        stats->eligible_unique_styles =
            static_cast<std::uint64_t>(eligible.size());

        for (std::size_t index = 1U; index < eligible.size(); ++index) {
            EligibleStyleV1 value = eligible[index];
            std::size_t insertion = index;
            while (insertion > 0U) {
                if (!consume_work(config, stats, 1U)) {
                    return set_error(
                        error,
                        CssOffscreenStyleSelectionErrorKindV1::WorkBudgetExceeded,
                        index,
                        "CSS offscreen style ordering exceeded work budget");
                }
                if (!before(value, eligible[insertion - 1U])) {
                    break;
                }
                eligible[insertion] = eligible[insertion - 1U];
                --insertion;
            }
            eligible[insertion] = value;
        }

        const std::size_t selected =
            eligible.size() < config.maximum_selected_styles
                ? eligible.size()
                : config.maximum_selected_styles;
        candidate_output.terminal_nodes.reserve(selected);
        for (std::size_t index = 0U; index < selected; ++index) {
            candidate_output.terminal_nodes.push_back(
                eligible[index].terminal_node);
        }

        stats->selected_styles =
            static_cast<std::uint64_t>(
                candidate_output.terminal_nodes.size());
        output->release();
        output->terminal_nodes.swap(candidate_output.terminal_nodes);
        return true;
    } catch (const std::bad_alloc&) {
        return set_error(
            error,
            CssOffscreenStyleSelectionErrorKindV1::AllocationFailure,
            0U,
            "CSS offscreen style selection allocation rejected by bounded memory resource");
    } catch (...) {
        return set_error(
            error,
            CssOffscreenStyleSelectionErrorKindV1::AllocationFailure,
            0U,
            "CSS offscreen style selection allocation or container operation failed");
    }
}

} // namespace zevryon::style
