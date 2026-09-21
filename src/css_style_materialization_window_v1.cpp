#include "css_style_materialization_window_v1.hpp"

#include <algorithm>
#include <string_view>

namespace zevryon::style {
namespace {

bool set_error(
    CssStyleMaterializationWindowErrorV1* error,
    CssStyleMaterializationWindowErrorKindV1 kind,
    std::size_t document_index,
    CssStyleMaterializationErrorKindV1 materialization_kind,
    std::string_view message) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->document_index = document_index;
        error->materialization_kind = materialization_kind;
        try {
            error->message.assign(message.data(), message.size());
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

} // namespace

bool CssStyleMaterializationWindowConfigV1::valid() const noexcept {
    if (maximum_visible_nodes == 0U ||
        maximum_visible_nodes > kMaximumVisibleNodesLimit ||
        offscreen_before_nodes >
            kMaximumOffscreenNodesPerSideLimit ||
        offscreen_after_nodes >
            kMaximumOffscreenNodesPerSideLimit ||
        maximum_selection_work_units == 0U ||
        maximum_selection_work_units >
            kMaximumSelectionWorkUnitsLimit ||
        !materialization.valid()) {
        return false;
    }

    const std::size_t maximum_window =
        maximum_visible_nodes +
        offscreen_before_nodes +
        offscreen_after_nodes;
    return maximum_window <= materialization.maximum_requests;
}

const char* css_style_materialization_window_error_kind_name_v1(
    CssStyleMaterializationWindowErrorKindV1 kind) noexcept {
    switch (kind) {
    case CssStyleMaterializationWindowErrorKindV1::None:
        return "none";
    case CssStyleMaterializationWindowErrorKindV1::InvalidConfiguration:
        return "invalid-configuration";
    case CssStyleMaterializationWindowErrorKindV1::InvalidVisibleRange:
        return "invalid-visible-range";
    case CssStyleMaterializationWindowErrorKindV1::VisibleNodeLimitExceeded:
        return "visible-node-limit-exceeded";
    case CssStyleMaterializationWindowErrorKindV1::SelectionWorkBudgetExceeded:
        return "selection-work-budget-exceeded";
    case CssStyleMaterializationWindowErrorKindV1::MaterializationFailure:
        return "materialization-failure";
    }
    return "unknown";
}

bool materialize_css_style_window_v1(
    const CssComputedStyleDagV1& dag,
    std::span<const std::uint32_t> document_terminal_nodes,
    std::size_t visible_begin,
    std::size_t visible_end,
    CssStyleMaterializationWindowConfigV1 config,
    CssStyleMaterializationBatchV1* output,
    CssStyleMaterializationWindowStatsV1* stats,
    CssStyleMaterializationWindowErrorV1* error) noexcept {
    if (stats != nullptr) {
        *stats = CssStyleMaterializationWindowStatsV1{};
    }
    if (error != nullptr) {
        error->kind = CssStyleMaterializationWindowErrorKindV1::None;
        error->document_index = 0U;
        error->materialization_kind =
            CssStyleMaterializationErrorKindV1::None;
        error->message.clear();
    }

    if (output == nullptr || stats == nullptr || error == nullptr ||
        output->resource() == nullptr || !config.valid()) {
        return set_error(
            error,
            CssStyleMaterializationWindowErrorKindV1::InvalidConfiguration,
            0U,
            CssStyleMaterializationErrorKindV1::None,
            "CSS style materialization window output, stats, error and valid configuration are required");
    }

    stats->document_nodes =
        static_cast<std::uint64_t>(
            document_terminal_nodes.size());

    if (visible_begin > visible_end ||
        visible_end > document_terminal_nodes.size()) {
        return set_error(
            error,
            CssStyleMaterializationWindowErrorKindV1::InvalidVisibleRange,
            visible_begin,
            CssStyleMaterializationErrorKindV1::None,
            "CSS style materialization visible range is outside document terminal span");
    }

    const std::size_t visible_count =
        visible_end - visible_begin;
    if (visible_count > config.maximum_visible_nodes) {
        return set_error(
            error,
            CssStyleMaterializationWindowErrorKindV1::VisibleNodeLimitExceeded,
            visible_begin,
            CssStyleMaterializationErrorKindV1::None,
            "CSS style materialization visible range exceeds configured node limit");
    }

    const std::size_t before =
        std::min(
            config.offscreen_before_nodes,
            visible_begin);
    const std::size_t available_after =
        document_terminal_nodes.size() - visible_end;
    const std::size_t after =
        std::min(
            config.offscreen_after_nodes,
            available_after);

    const std::size_t selected_begin =
        visible_begin - before;
    const std::size_t selected_count =
        before + visible_count + after;
    const std::size_t selected_end =
        selected_begin + selected_count;

    if (selected_count >
        static_cast<std::size_t>(
            config.maximum_selection_work_units)) {
        return set_error(
            error,
            CssStyleMaterializationWindowErrorKindV1::SelectionWorkBudgetExceeded,
            selected_begin,
            CssStyleMaterializationErrorKindV1::None,
            "CSS style materialization selected window exceeds selection work budget");
    }

    stats->visible_nodes =
        static_cast<std::uint64_t>(visible_count);
    stats->offscreen_before_nodes =
        static_cast<std::uint64_t>(before);
    stats->offscreen_after_nodes =
        static_cast<std::uint64_t>(after);
    stats->selected_requests =
        static_cast<std::uint64_t>(selected_count);
    stats->selection_work_units =
        static_cast<std::uint64_t>(selected_count);
    stats->selected_document_begin = selected_begin;
    stats->selected_document_end = selected_end;

    CssStyleMaterializationStatsV1 materialization_stats;
    CssStyleMaterializationErrorV1 materialization_error;
    const std::span<const std::uint32_t> selected =
        document_terminal_nodes.subspan(
            selected_begin,
            selected_count);

    if (!materialize_css_style_nodes_v1(
            dag,
            selected,
            config.materialization,
            output,
            &materialization_stats,
            &materialization_error)) {
        stats->materialization = materialization_stats;
        const std::size_t failed_document_index =
            selected_begin +
            std::min(
                materialization_error.request_index,
                selected_count);
        return set_error(
            error,
            CssStyleMaterializationWindowErrorKindV1::MaterializationFailure,
            failed_document_index,
            materialization_error.kind,
            materialization_error.message);
    }

    stats->materialization = materialization_stats;
    return true;
}

} // namespace zevryon::style
