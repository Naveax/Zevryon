#include "css_style_materialization_window_v1.hpp"

#include <algorithm>
#include <string_view>

namespace zevryon::style {
namespace {

bool set_error(
    CssStyleMaterializationWindowErrorV1* error,
    CssStyleMaterializationWindowErrorKindV1 kind,
    std::uint64_t document_index,
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
    case CssStyleMaterializationWindowErrorKindV1::CandidateWindowLimitExceeded:
        return "candidate-window-limit-exceeded";
    case CssStyleMaterializationWindowErrorKindV1::InvalidCandidateWindow:
        return "invalid-candidate-window";
    case CssStyleMaterializationWindowErrorKindV1::SelectionWorkBudgetExceeded:
        return "selection-work-budget-exceeded";
    case CssStyleMaterializationWindowErrorKindV1::MaterializationFailure:
        return "materialization-failure";
    }
    return "unknown";
}

bool materialize_css_style_window_v1(
    const CssComputedStyleDagV1& dag,
    std::span<const std::uint32_t> candidate_terminal_nodes,
    std::uint64_t candidate_document_begin,
    std::uint64_t document_node_count,
    std::uint64_t visible_begin,
    std::uint64_t visible_end,
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

    stats->document_nodes = document_node_count;

    const std::size_t maximum_window =
        config.maximum_visible_nodes +
        config.offscreen_before_nodes +
        config.offscreen_after_nodes;
    if (candidate_terminal_nodes.size() > maximum_window) {
        return set_error(
            error,
            CssStyleMaterializationWindowErrorKindV1::CandidateWindowLimitExceeded,
            candidate_document_begin,
            CssStyleMaterializationErrorKindV1::None,
            "CSS style materialization candidate span exceeds configured worst-case window");
    }

    const std::uint64_t candidate_count =
        static_cast<std::uint64_t>(candidate_terminal_nodes.size());
    if (candidate_document_begin > document_node_count ||
        candidate_count >
            document_node_count - candidate_document_begin) {
        return set_error(
            error,
            CssStyleMaterializationWindowErrorKindV1::InvalidCandidateWindow,
            candidate_document_begin,
            CssStyleMaterializationErrorKindV1::None,
            "CSS style materialization candidate span lies outside logical document");
    }

    const std::uint64_t candidate_document_end =
        candidate_document_begin + candidate_count;
    stats->candidate_nodes = candidate_count;
    stats->candidate_document_begin = candidate_document_begin;
    stats->candidate_document_end = candidate_document_end;

    if (visible_begin > visible_end ||
        visible_end > document_node_count) {
        return set_error(
            error,
            CssStyleMaterializationWindowErrorKindV1::InvalidVisibleRange,
            visible_begin,
            CssStyleMaterializationErrorKindV1::None,
            "CSS style materialization visible range is outside logical document");
    }

    const std::uint64_t visible_count =
        visible_end - visible_begin;
    if (visible_count >
        static_cast<std::uint64_t>(config.maximum_visible_nodes)) {
        return set_error(
            error,
            CssStyleMaterializationWindowErrorKindV1::VisibleNodeLimitExceeded,
            visible_begin,
            CssStyleMaterializationErrorKindV1::None,
            "CSS style materialization visible range exceeds configured node limit");
    }

    const std::uint64_t before =
        std::min<std::uint64_t>(
            static_cast<std::uint64_t>(
                config.offscreen_before_nodes),
            visible_begin);
    const std::uint64_t available_after =
        document_node_count - visible_end;
    const std::uint64_t after =
        std::min<std::uint64_t>(
            static_cast<std::uint64_t>(
                config.offscreen_after_nodes),
            available_after);

    const std::uint64_t selected_begin =
        visible_begin - before;
    const std::uint64_t selected_count =
        before + visible_count + after;
    const std::uint64_t selected_end =
        visible_end + after;

    if (selected_count > config.maximum_selection_work_units) {
        return set_error(
            error,
            CssStyleMaterializationWindowErrorKindV1::SelectionWorkBudgetExceeded,
            selected_begin,
            CssStyleMaterializationErrorKindV1::None,
            "CSS style materialization selected window exceeds selection work budget");
    }

    if (candidate_document_begin > selected_begin ||
        candidate_document_end < selected_end) {
        return set_error(
            error,
            CssStyleMaterializationWindowErrorKindV1::InvalidCandidateWindow,
            selected_begin,
            CssStyleMaterializationErrorKindV1::None,
            "CSS style materialization candidate span does not cover selected visible plus lookahead range");
    }

    const std::uint64_t local_offset_u64 =
        selected_begin - candidate_document_begin;
    const std::size_t local_offset =
        static_cast<std::size_t>(local_offset_u64);
    const std::size_t local_count =
        static_cast<std::size_t>(selected_count);

    stats->visible_nodes = visible_count;
    stats->offscreen_before_nodes = before;
    stats->offscreen_after_nodes = after;
    stats->selected_requests = selected_count;
    stats->selection_work_units = selected_count;
    stats->selected_document_begin = selected_begin;
    stats->selected_document_end = selected_end;

    CssStyleMaterializationStatsV1 materialization_stats;
    CssStyleMaterializationErrorV1 materialization_error;
    const std::span<const std::uint32_t> selected =
        candidate_terminal_nodes.subspan(
            local_offset,
            local_count);

    if (!materialize_css_style_nodes_v1(
            dag,
            selected,
            config.materialization,
            output,
            &materialization_stats,
            &materialization_error)) {
        stats->materialization = materialization_stats;
        std::uint64_t failed_document_index = selected_begin;
        if (local_count != 0U) {
            const std::size_t bounded_request_index =
                std::min(
                    materialization_error.request_index,
                    local_count - 1U);
            failed_document_index +=
                static_cast<std::uint64_t>(
                    bounded_request_index);
        }
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
