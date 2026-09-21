#pragma once

#include "css_style_materialization_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace zevryon::style {

struct CssStyleMaterializationWindowConfigV1 {
    static constexpr std::size_t kMaximumVisibleNodesLimit = 65'536U;
    static constexpr std::size_t kMaximumOffscreenNodesPerSideLimit = 65'536U;
    static constexpr std::uint64_t kMaximumSelectionWorkUnitsLimit =
        65'536U;

    std::size_t maximum_visible_nodes{2048U};
    std::size_t offscreen_before_nodes{256U};
    std::size_t offscreen_after_nodes{256U};
    std::uint64_t maximum_selection_work_units{4096U};
    CssStyleMaterializationConfigV1 materialization{};

    bool valid() const noexcept;
};

enum class CssStyleMaterializationWindowErrorKindV1 : std::uint8_t {
    None = 0,
    InvalidConfiguration,
    InvalidVisibleRange,
    VisibleNodeLimitExceeded,
    SelectionWorkBudgetExceeded,
    MaterializationFailure,
};

struct CssStyleMaterializationWindowErrorV1 {
    CssStyleMaterializationWindowErrorKindV1 kind{
        CssStyleMaterializationWindowErrorKindV1::None};
    std::size_t document_index{0U};
    CssStyleMaterializationErrorKindV1 materialization_kind{
        CssStyleMaterializationErrorKindV1::None};
    std::string message;
};

struct CssStyleMaterializationWindowStatsV1 {
    std::uint64_t document_nodes{0U};
    std::uint64_t visible_nodes{0U};
    std::uint64_t offscreen_before_nodes{0U};
    std::uint64_t offscreen_after_nodes{0U};
    std::uint64_t selected_requests{0U};
    std::uint64_t selection_work_units{0U};
    std::size_t selected_document_begin{0U};
    std::size_t selected_document_end{0U};
    CssStyleMaterializationStatsV1 materialization{};
};

const char* css_style_materialization_window_error_kind_name_v1(
    CssStyleMaterializationWindowErrorKindV1 kind) noexcept;

// Materializes a bounded document-order window around a half-open visible
// range. The function does not scan document_terminal_nodes. It computes one
// clipped contiguous subspan using the configured before/after lookahead and
// delegates only that subspan to materialize_css_style_nodes_v1().
//
// output.request_style_indices[i] corresponds to document node
// stats.selected_document_begin + i.
bool materialize_css_style_window_v1(
    const CssComputedStyleDagV1& dag,
    std::span<const std::uint32_t> document_terminal_nodes,
    std::size_t visible_begin,
    std::size_t visible_end,
    CssStyleMaterializationWindowConfigV1 config,
    CssStyleMaterializationBatchV1* output,
    CssStyleMaterializationWindowStatsV1* stats,
    CssStyleMaterializationWindowErrorV1* error) noexcept;

} // namespace zevryon::style
