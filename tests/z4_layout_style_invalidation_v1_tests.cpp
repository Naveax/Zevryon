#include "z4_layout_style_invalidation_v1.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory_resource>
#include <string_view>

namespace {

using namespace zevryon::layout;
using namespace zevryon::style;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: Z4 style invalidation: " << message << '\n';
        return false;
    }
    return true;
}

CssSemanticStyleWindowV1 make_window(
    std::pmr::memory_resource* memory,
    std::uint64_t begin,
    std::uint64_t document_nodes,
    std::initializer_list<std::uint32_t> terminals) {
    CssSemanticStyleWindowV1 window(memory);
    window.document_begin = begin;
    window.document_end =
        begin + static_cast<std::uint64_t>(terminals.size());
    window.document_node_count = document_nodes;
    for (const std::uint32_t terminal : terminals) {
        window.terminal_nodes.push_back(terminal);
    }
    return window;
}

bool test_exact_changed_ranges_and_stats() {
    std::pmr::monotonic_buffer_resource memory;
    CssComputedStyleDagV1 dag(&memory);
    dag.nodes.resize(5U);

    auto previous = make_window(
        &memory, 100U, 1'000'000U, {0U, 1U, 1U, 2U, 2U, 3U, 3U, 4U});
    auto current = make_window(
        &memory, 100U, 1'000'000U, {0U, 2U, 3U, 2U, 4U, 4U, 3U, 4U});

    LayoutStyleInvalidationResultV1 output(&memory);
    LayoutStyleInvalidationStatsV1 stats;
    LayoutStyleInvalidationErrorV1 error;
    if (!require(
            compute_layout_style_invalidation_v1(
                dag, previous, current, {}, &output, &stats, &error),
            "exact range oracle must succeed")) {
        return false;
    }

    return require(
        output.ranges.size() == 2U &&
            output.ranges[0] ==
                LayoutStyleInvalidationRangeV1{101U, 103U} &&
            output.ranges[1] ==
                LayoutStyleInvalidationRangeV1{104U, 106U} &&
            stats.nodes_considered == 8U &&
            stats.changed_nodes == 4U &&
            stats.output_ranges == 2U &&
            stats.work_units == 8U,
        "changes must coalesce into exact absolute half-open ranges");
}

bool test_unchanged_window_emits_no_ranges() {
    std::pmr::monotonic_buffer_resource memory;
    CssComputedStyleDagV1 dag(&memory);
    dag.nodes.resize(3U);

    auto previous = make_window(&memory, 5U, 10U, {0U, 1U, 2U});
    auto current = make_window(&memory, 5U, 10U, {0U, 1U, 2U});

    LayoutStyleInvalidationResultV1 output(&memory);
    LayoutStyleInvalidationStatsV1 stats;
    LayoutStyleInvalidationErrorV1 error;
    return require(
        compute_layout_style_invalidation_v1(
            dag, previous, current, {}, &output, &stats, &error) &&
            output.ranges.empty() &&
            stats.changed_nodes == 0U &&
            stats.nodes_considered == 3U,
        "unchanged canonical style identities must produce no invalidation");
}

bool test_failures_preserve_prior_output() {
    std::pmr::monotonic_buffer_resource memory;
    CssComputedStyleDagV1 dag(&memory);
    dag.nodes.resize(3U);

    LayoutStyleInvalidationResultV1 output(&memory);
    output.ranges.push_back({900U, 901U});
    LayoutStyleInvalidationStatsV1 stats;
    LayoutStyleInvalidationErrorV1 error;

    auto previous = make_window(&memory, 0U, 10U, {0U, 1U, 2U});
    auto invalid_terminal = make_window(&memory, 0U, 10U, {0U, 9U, 2U});
    if (!require(
            !compute_layout_style_invalidation_v1(
                dag, previous, invalid_terminal, {}, &output, &stats, &error) &&
                error.kind ==
                    LayoutStyleInvalidationErrorKindV1::InvalidStyleTerminal &&
                output.ranges.size() == 1U &&
                output.ranges[0] ==
                    LayoutStyleInvalidationRangeV1{900U, 901U},
            "invalid terminal must fail atomically")) {
        return false;
    }

    auto disjoint = make_window(&memory, 0U, 10U, {1U, 1U, 1U});
    LayoutStyleInvalidationConfigV1 tiny_ranges;
    tiny_ranges.maximum_ranges = 1U;
    if (!require(
            !compute_layout_style_invalidation_v1(
                dag, previous, disjoint, tiny_ranges, &output, &stats, &error) &&
                error.kind ==
                    LayoutStyleInvalidationErrorKindV1::RangeLimitExceeded &&
                output.ranges.size() == 1U &&
                output.ranges[0] ==
                    LayoutStyleInvalidationRangeV1{900U, 901U},
            "range overflow must preserve prior output")) {
        return false;
    }

    LayoutStyleInvalidationConfigV1 tiny_work;
    tiny_work.maximum_work_units = 2U;
    return require(
        !compute_layout_style_invalidation_v1(
            dag, previous, disjoint, tiny_work, &output, &stats, &error) &&
            error.kind ==
                LayoutStyleInvalidationErrorKindV1::WorkBudgetExceeded &&
            stats.work_units == 2U &&
            output.ranges.size() == 1U,
        "work budget must fail before unbounded window scan");
}

} // namespace

int main() {
    if (!test_exact_changed_ranges_and_stats() ||
        !test_unchanged_window_emits_no_ranges() ||
        !test_failures_preserve_prior_output()) {
        return 1;
    }

    std::cout << "Z4 layout style invalidation v1 tests passed\n";
    return 0;
}
