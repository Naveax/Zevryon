#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::style {

struct CssOffscreenStyleCandidateV1 {
    std::uint32_t terminal_node{0U};
    std::uint32_t distance_css_px{0U};
    bool intersects_viewport{false};
};

struct CssOffscreenStyleSelectionV1 {
    explicit CssOffscreenStyleSelectionV1(
        std::pmr::memory_resource* memory);

    std::pmr::vector<std::uint32_t> terminal_nodes;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;
};

struct CssOffscreenStyleSelectionConfigV1 {
    static constexpr std::size_t kMaximumCandidatesLimit = 16'384U;
    static constexpr std::size_t kMaximumSelectedStylesLimit = 16'384U;
    static constexpr std::uint32_t kMaximumLookaheadCssPxLimit = 1'000'000U;
    static constexpr std::uint64_t kMaximumWorkUnitsLimit =
        64U * 1024U * 1024U;

    std::size_t maximum_candidates{4096U};
    std::size_t maximum_selected_styles{512U};
    std::uint32_t lookahead_css_px{2048U};
    std::uint64_t maximum_work_units{8U * 1024U * 1024U};

    bool valid() const noexcept;
};

enum class CssOffscreenStyleSelectionErrorKindV1 : std::uint8_t {
    None = 0,
    InvalidConfiguration,
    CandidateLimitExceeded,
    WorkBudgetExceeded,
    AllocationFailure,
};

struct CssOffscreenStyleSelectionErrorV1 {
    CssOffscreenStyleSelectionErrorKindV1 kind{
        CssOffscreenStyleSelectionErrorKindV1::None};
    std::size_t candidate_index{0U};
    std::string message;
};

struct CssOffscreenStyleSelectionStatsV1 {
    std::uint64_t candidates_considered{0U};
    std::uint64_t visible_candidates_skipped{0U};
    std::uint64_t outside_lookahead_skipped{0U};
    std::uint64_t duplicate_terminal_candidates{0U};
    std::uint64_t eligible_unique_styles{0U};
    std::uint64_t selected_styles{0U};
    std::uint64_t work_units{0U};
};

const char* css_offscreen_style_selection_error_kind_name_v1(
    CssOffscreenStyleSelectionErrorKindV1 kind) noexcept;

// Selects a deterministic bounded set of offscreen terminal style IDs.
//
// Visible candidates are deliberately excluded from this pre-materialization
// policy. Offscreen candidates beyond the configured lookahead are ignored.
// Duplicate terminal IDs collapse to their nearest offscreen occurrence.
// Eligible unique styles are ordered by distance, then terminal ID, and the
// result is truncated to maximum_selected_styles.
bool select_css_offscreen_style_nodes_v1(
    std::span<const CssOffscreenStyleCandidateV1> candidates,
    CssOffscreenStyleSelectionConfigV1 config,
    CssOffscreenStyleSelectionV1* output,
    CssOffscreenStyleSelectionStatsV1* stats,
    CssOffscreenStyleSelectionErrorV1* error) noexcept;

} // namespace zevryon::style
