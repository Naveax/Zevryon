#include "css_semantic_style_bridge_v1.hpp"

#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace zevryon::style;
using zevryon::massivedoc::ZenithSemanticAttribute;
using zevryon::massivedoc::ZenithSemanticNode;
using zevryon::massivedoc::ZenithSemanticNodeWindowResult;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr
            << "FAILED: Z3 semantic style bridge: "
            << message << '\n';
        return false;
    }
    return true;
}

ZenithSemanticNode make_node(
    std::string tag,
    std::vector<std::pair<std::string, std::string>> attributes,
    std::string inline_style = {}) {
    ZenithSemanticNode node;
    node.tag = std::move(tag);
    node.style = std::move(inline_style);
    for (auto& [name, value] : attributes) {
        ZenithSemanticAttribute attribute;
        attribute.name = std::move(name);
        attribute.value = std::move(value);
        node.attributes.push_back(std::move(attribute));
    }
    node.record.attribute_count =
        static_cast<std::uint32_t>(node.attributes.size());
    return node;
}

bool parse_sheet(
    std::string_view source,
    CssStylesheetV1* sheet) {
    CssParserV1Stats stats;
    CssParserV1Error error;
    return parse_css_stylesheet_v1(
        source,
        CssParserV1Config{},
        sheet,
        &stats,
        &error);
}

bool test_bounded_semantic_window_to_terminal_styles() {
    std::pmr::monotonic_buffer_resource stylesheet_memory;
    CssStylesheetV1 sheet(&stylesheet_memory);
    if (!require(
            parse_sheet(
                "div{color:red;}"
                ".hot{color:blue;}"
                "[data-x=one]{margin:2px;}",
                &sheet),
            "bridge stylesheet must parse")) {
        return false;
    }

    ZenithSemanticNodeWindowResult window;
    window.start_ordinal = 100U;
    window.next_ordinal = 103U;
    window.arena_node_count = 1'000'000U;
    window.nodes.push_back(
        make_node(
            "div",
            {{"class", "hot"}, {"data-x", "one"}}));
    window.nodes.push_back(
        make_node(
            "div",
            {{"class", "hot"}, {"data-x", "one"}}));
    window.nodes.push_back(make_node("span", {}));
    window.attribute_count = 4U;

    std::pmr::monotonic_buffer_resource style_memory;
    CssComputedStyleDagV1 dag(&style_memory);
    CssSemanticStyleWindowV1 output(&style_memory);
    CssSemanticStyleBridgeStatsV1 stats;
    CssSemanticStyleBridgeErrorV1 error;

    if (!require(
            compute_css_style_terminals_for_semantic_window_v1(
                sheet,
                window,
                CssSemanticStyleBridgeConfigV1{},
                &dag,
                &output,
                &stats,
                &error),
            "bounded semantic window must compute style terminals")) {
        return false;
    }

    if (!require(
            output.document_begin == 100U &&
                output.document_end == 103U &&
                output.document_node_count == 1'000'000U &&
                output.terminal_nodes.size() == 3U,
            "bridge output must preserve absolute bounded window identity")) {
        return false;
    }
    if (!require(
            output.terminal_nodes[0] ==
                    output.terminal_nodes[1] &&
                output.terminal_nodes[0] != 0U &&
                output.terminal_nodes[2] == 0U,
            "equal semantic nodes must reuse one terminal and unmatched span must use empty root")) {
        return false;
    }
    if (!require(
            dag.nodes[
                static_cast<std::size_t>(
                    output.terminal_nodes[0])].depth == 2U,
            "matched .hot plus data attribute style must contain two winning properties")) {
        return false;
    }
    if (!require(
            stats.nodes_considered == 3U &&
                stats.nodes_styled == 3U &&
                stats.attributes_considered == 4U &&
                stats.preflight_work_units == 7U &&
                stats.work_units >= stats.preflight_work_units,
            "bridge bounded-work statistics must be exact at preflight boundary")) {
        return false;
    }
    return true;
}

bool test_inline_style_rejects_before_dag_mutation() {
    std::pmr::monotonic_buffer_resource stylesheet_memory;
    CssStylesheetV1 sheet(&stylesheet_memory);
    if (!require(
            parse_sheet("div{color:red;}", &sheet),
            "inline rejection stylesheet must parse")) {
        return false;
    }

    std::pmr::monotonic_buffer_resource style_memory;
    CssComputedStyleDagV1 dag(&style_memory);
    CssSemanticStyleWindowV1 output(&style_memory);
    CssSemanticStyleBridgeStatsV1 stats;
    CssSemanticStyleBridgeErrorV1 error;

    ZenithSemanticNodeWindowResult baseline;
    baseline.start_ordinal = 0U;
    baseline.next_ordinal = 1U;
    baseline.arena_node_count = 10U;
    baseline.nodes.push_back(make_node("div", {}));
    if (!require(
            compute_css_style_terminals_for_semantic_window_v1(
                sheet,
                baseline,
                CssSemanticStyleBridgeConfigV1{},
                &dag,
                &output,
                &stats,
                &error),
            "inline rejection baseline must succeed")) {
        return false;
    }

    const std::size_t nodes_before = dag.nodes.size();
    const std::size_t text_before = dag.text.size();
    const std::vector<std::uint32_t> output_before(
        output.terminal_nodes.begin(),
        output.terminal_nodes.end());

    ZenithSemanticNodeWindowResult invalid;
    invalid.start_ordinal = 5U;
    invalid.next_ordinal = 7U;
    invalid.arena_node_count = 10U;
    invalid.nodes.push_back(make_node("div", {}));
    invalid.nodes.push_back(
        make_node(
            "div",
            {{"style", "color:green"}},
            "color:green"));
    invalid.attribute_count = 1U;

    if (!require(
            !compute_css_style_terminals_for_semantic_window_v1(
                sheet,
                invalid,
                CssSemanticStyleBridgeConfigV1{},
                &dag,
                &output,
                &stats,
                &error),
            "non-empty inline style must fail closed")) {
        return false;
    }
    if (!require(
            error.kind ==
                    CssSemanticStyleBridgeErrorKindV1::InlineStyleUnsupported &&
                error.node_index == 1U &&
                error.document_ordinal == 6U,
            "inline style rejection must identify exact bounded node")) {
        return false;
    }
    if (!require(
            dag.nodes.size() == nodes_before &&
                dag.text.size() == text_before &&
                std::vector<std::uint32_t>(
                    output.terminal_nodes.begin(),
                    output.terminal_nodes.end()) ==
                    output_before,
            "preflight inline rejection must preserve DAG and prior output")) {
        return false;
    }
    return true;
}

bool test_window_and_shared_work_bounds_fail_closed() {
    std::pmr::monotonic_buffer_resource stylesheet_memory;
    CssStylesheetV1 sheet(&stylesheet_memory);
    if (!require(
            parse_sheet("div{a:1;}", &sheet),
            "bound stylesheet must parse")) {
        return false;
    }

    std::pmr::monotonic_buffer_resource style_memory;
    CssComputedStyleDagV1 dag(&style_memory);
    CssSemanticStyleWindowV1 output(&style_memory);
    output.document_begin = 900U;
    output.document_end = 901U;
    output.document_node_count = 1000U;
    output.terminal_nodes.push_back(777U);

    CssSemanticStyleBridgeStatsV1 stats;
    CssSemanticStyleBridgeErrorV1 error;

    ZenithSemanticNodeWindowResult malformed;
    malformed.start_ordinal = 10U;
    malformed.next_ordinal = 12U;
    malformed.arena_node_count = 100U;
    malformed.nodes.push_back(make_node("div", {}));

    if (!require(
            !compute_css_style_terminals_for_semantic_window_v1(
                sheet,
                malformed,
                CssSemanticStyleBridgeConfigV1{},
                &dag,
                &output,
                &stats,
                &error) &&
                error.kind ==
                    CssSemanticStyleBridgeErrorKindV1::InvalidWindow,
            "inconsistent Z8 window must fail closed")) {
        return false;
    }

    ZenithSemanticNodeWindowResult one;
    one.start_ordinal = 50U;
    one.next_ordinal = 51U;
    one.arena_node_count = 100U;
    one.nodes.push_back(make_node("div", {}));

    CssSemanticStyleBridgeConfigV1 tiny_work;
    tiny_work.maximum_work_units = 1U;
    if (!require(
            !compute_css_style_terminals_for_semantic_window_v1(
                sheet,
                one,
                tiny_work,
                &dag,
                &output,
                &stats,
                &error) &&
                error.kind ==
                    CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
            "shared bridge work budget must fail before unbounded nested work")) {
        return false;
    }
    if (!require(
            output.document_begin == 900U &&
                output.document_end == 901U &&
                output.document_node_count == 1000U &&
                output.terminal_nodes.size() == 1U &&
                output.terminal_nodes[0] == 777U,
            "bridge failures must preserve prior published terminal window")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_bounded_semantic_window_to_terminal_styles() ||
        !test_inline_style_rejects_before_dag_mutation() ||
        !test_window_and_shared_work_bounds_fail_closed()) {
        return 1;
    }

    std::cout
        << "Z3 semantic style bridge v1 tests passed\n";
    return 0;
}
