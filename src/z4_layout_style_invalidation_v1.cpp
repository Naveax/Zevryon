#include "z4_layout_style_invalidation_v1.hpp"

#include <algorithm>
#include <new>
#include <string_view>

namespace zevryon::layout {
namespace {

bool set_error(
    LayoutStyleInvalidationErrorV1* error,
    LayoutStyleInvalidationErrorKindV1 kind,
    std::size_t node_index,
    std::uint64_t document_ordinal,
    std::string_view message) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->node_index = node_index;
        error->document_ordinal = document_ordinal;
        try {
            error->message.assign(message.data(), message.size());
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool valid_window_shape(
    const zevryon::style::CssSemanticStyleWindowV1& window) noexcept {
    if (window.document_begin > window.document_end ||
        window.document_end > window.document_node_count) {
        return false;
    }
    const std::uint64_t span =
        window.document_end - window.document_begin;
    return span ==
        static_cast<std::uint64_t>(window.terminal_nodes.size());
}

bool charge_work(
    LayoutStyleInvalidationConfigV1 config,
    LayoutStyleInvalidationStatsV1* stats,
    std::uint64_t units) noexcept {
    if (stats == nullptr ||
        stats->work_units > config.maximum_work_units ||
        units > config.maximum_work_units - stats->work_units) {
        return false;
    }
    stats->work_units += units;
    return true;
}

} // namespace

bool LayoutStyleInvalidationConfigV1::valid() const noexcept {
    return maximum_nodes > 0U &&
        maximum_nodes <= kMaximumNodesLimit &&
        maximum_ranges > 0U &&
        maximum_ranges <= kMaximumRangesLimit &&
        maximum_work_units > 0U &&
        maximum_work_units <= kMaximumWorkUnitsLimit;
}

LayoutStyleInvalidationResultV1::LayoutStyleInvalidationResultV1(
    std::pmr::memory_resource* memory)
    : ranges(memory) {}

std::pmr::memory_resource*
LayoutStyleInvalidationResultV1::resource() const noexcept {
    return ranges.get_allocator().resource();
}

void LayoutStyleInvalidationResultV1::release() noexcept {
    std::pmr::vector<LayoutStyleInvalidationRangeV1> empty(resource());
    ranges.swap(empty);
}

const char* layout_style_invalidation_error_kind_name_v1(
    LayoutStyleInvalidationErrorKindV1 kind) noexcept {
    switch (kind) {
    case LayoutStyleInvalidationErrorKindV1::None:
        return "none";
    case LayoutStyleInvalidationErrorKindV1::InvalidConfiguration:
        return "invalid-configuration";
    case LayoutStyleInvalidationErrorKindV1::InvalidWindow:
        return "invalid-window";
    case LayoutStyleInvalidationErrorKindV1::NodeLimitExceeded:
        return "node-limit-exceeded";
    case LayoutStyleInvalidationErrorKindV1::RangeLimitExceeded:
        return "range-limit-exceeded";
    case LayoutStyleInvalidationErrorKindV1::WorkBudgetExceeded:
        return "work-budget-exceeded";
    case LayoutStyleInvalidationErrorKindV1::InvalidStyleTerminal:
        return "invalid-style-terminal";
    case LayoutStyleInvalidationErrorKindV1::AllocationFailure:
        return "allocation-failure";
    }
    return "unknown";
}

bool compute_layout_style_invalidation_v1(
    const zevryon::style::CssComputedStyleDagV1& dag,
    const zevryon::style::CssSemanticStyleWindowV1& previous,
    const zevryon::style::CssSemanticStyleWindowV1& current,
    LayoutStyleInvalidationConfigV1 config,
    LayoutStyleInvalidationResultV1* output,
    LayoutStyleInvalidationStatsV1* stats,
    LayoutStyleInvalidationErrorV1* error) noexcept {
    if (stats != nullptr) {
        *stats = LayoutStyleInvalidationStatsV1{};
    }
    if (error != nullptr) {
        *error = LayoutStyleInvalidationErrorV1{};
    }

    if (output == nullptr || stats == nullptr || error == nullptr ||
        output->resource() == nullptr || !config.valid()) {
        return set_error(
            error,
            LayoutStyleInvalidationErrorKindV1::InvalidConfiguration,
            0U,
            current.document_begin,
            "Z4 layout style invalidation requires output, stats, error and valid configuration");
    }

    if (!valid_window_shape(previous) ||
        !valid_window_shape(current) ||
        previous.document_begin != current.document_begin ||
        previous.document_end != current.document_end ||
        previous.document_node_count != current.document_node_count) {
        return set_error(
            error,
            LayoutStyleInvalidationErrorKindV1::InvalidWindow,
            0U,
            current.document_begin,
            "previous and current style windows must describe the same bounded document range");
    }

    const std::size_t node_count = current.terminal_nodes.size();
    if (node_count > config.maximum_nodes) {
        return set_error(
            error,
            LayoutStyleInvalidationErrorKindV1::NodeLimitExceeded,
            config.maximum_nodes,
            current.document_begin +
                static_cast<std::uint64_t>(config.maximum_nodes),
            "bounded style window exceeds Z4 invalidation node limit");
    }

    LayoutStyleInvalidationResultV1 candidate(output->resource());
    LayoutStyleInvalidationStatsV1 candidate_stats;

    try {
        candidate.ranges.reserve(
            std::min(node_count, config.maximum_ranges));

        bool range_open = false;
        LayoutStyleInvalidationRangeV1 open_range{};

        for (std::size_t index = 0U; index < node_count; ++index) {
            if (!charge_work(config, &candidate_stats, 1U)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    LayoutStyleInvalidationErrorKindV1::WorkBudgetExceeded,
                    index,
                    current.document_begin +
                        static_cast<std::uint64_t>(index),
                    "Z4 layout style invalidation exceeded comparison work budget");
            }
            ++candidate_stats.nodes_considered;

            const std::uint32_t previous_terminal =
                previous.terminal_nodes[index];
            const std::uint32_t current_terminal =
                current.terminal_nodes[index];
            if (static_cast<std::size_t>(previous_terminal) >= dag.nodes.size() ||
                static_cast<std::size_t>(current_terminal) >= dag.nodes.size()) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    LayoutStyleInvalidationErrorKindV1::InvalidStyleTerminal,
                    index,
                    current.document_begin +
                        static_cast<std::uint64_t>(index),
                    "style terminal is outside the supplied canonical DAG");
            }

            const bool changed =
                previous_terminal != current_terminal;
            if (!changed) {
                if (range_open) {
                    if (candidate.ranges.size() >= config.maximum_ranges) {
                        *stats = candidate_stats;
                        return set_error(
                            error,
                            LayoutStyleInvalidationErrorKindV1::RangeLimitExceeded,
                            index,
                            current.document_begin +
                                static_cast<std::uint64_t>(index),
                            "Z4 layout style invalidation exceeded output range limit");
                    }
                    candidate.ranges.push_back(open_range);
                    range_open = false;
                }
                continue;
            }

            ++candidate_stats.changed_nodes;
            const std::uint64_t ordinal =
                current.document_begin +
                static_cast<std::uint64_t>(index);
            if (!range_open) {
                open_range.document_begin = ordinal;
                open_range.document_end = ordinal + 1U;
                range_open = true;
            } else {
                open_range.document_end = ordinal + 1U;
            }
        }

        if (range_open) {
            if (candidate.ranges.size() >= config.maximum_ranges) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    LayoutStyleInvalidationErrorKindV1::RangeLimitExceeded,
                    node_count == 0U ? 0U : node_count - 1U,
                    current.document_end,
                    "Z4 layout style invalidation exceeded output range limit");
            }
            candidate.ranges.push_back(open_range);
        }

        candidate_stats.output_ranges =
            static_cast<std::uint64_t>(candidate.ranges.size());

        output->ranges.swap(candidate.ranges);
        *stats = candidate_stats;
        return true;
    } catch (const std::bad_alloc&) {
        *stats = candidate_stats;
        return set_error(
            error,
            LayoutStyleInvalidationErrorKindV1::AllocationFailure,
            0U,
            current.document_begin,
            "Z4 layout style invalidation allocation rejected");
    } catch (...) {
        *stats = candidate_stats;
        return set_error(
            error,
            LayoutStyleInvalidationErrorKindV1::AllocationFailure,
            0U,
            current.document_begin,
            "Z4 layout style invalidation container operation failed");
    }
}

} // namespace zevryon::layout
