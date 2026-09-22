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

std::string_view dag_value_for(
    const CssComputedStyleDagV1& dag,
    std::uint32_t terminal,
    std::string_view property) {
    while (terminal != kCssStyleDagNoNodeV1 &&
           static_cast<std::size_t>(terminal) < dag.nodes.size()) {
        const CssStyleDagNodeV1& node =
            dag.nodes[static_cast<std::size_t>(terminal)];
        if (dag.resolve(node.property) == property) {
            return dag.resolve(node.value);
        }
        terminal = node.parent;
    }
    return {};
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

bool test_inline_style_precedence_and_dag_integration() {
    std::pmr::monotonic_buffer_resource stylesheet_memory;
    CssStylesheetV1 sheet(&stylesheet_memory);
    if (!require(
            parse_sheet(
                "div{"
                "color:red!important;"
                "margin:1px;"
                "padding:3px;"
                "outline:1px!important;"
                "}",
                &sheet),
            "inline precedence stylesheet must parse")) {
        return false;
    }

    constexpr std::string_view inline_style =
        "color:blue;"
        "margin:2px!important;"
        "padding:4px;"
        "outline:2px!important;"
        "border:5px";

    ZenithSemanticNodeWindowResult window;
    window.start_ordinal = 5U;
    window.next_ordinal = 6U;
    window.arena_node_count = 10U;
    window.nodes.push_back(
        make_node(
            "div",
            {{"style", std::string(inline_style)}},
            std::string(inline_style)));
    window.attribute_count = 1U;

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
            "non-empty inline style must parse merge and intern")) {
        return false;
    }
    if (!require(
            output.terminal_nodes.size() == 1U &&
                output.terminal_nodes[0] !=
                    kCssStyleDagNoNodeV1,
            "inline style must publish one terminal identity")) {
        return false;
    }

    const std::uint32_t terminal =
        output.terminal_nodes[0];
    if (!require(
            dag.nodes[static_cast<std::size_t>(terminal)].depth == 5U,
            "merged author plus inline style must contain five winners")) {
        return false;
    }
    if (!require(
            dag_value_for(dag, terminal, "color") == "red",
            "author important must beat inline normal")) {
        return false;
    }
    if (!require(
            dag_value_for(dag, terminal, "margin") == "2px",
            "inline important must beat author normal")) {
        return false;
    }
    if (!require(
            dag_value_for(dag, terminal, "padding") == "4px",
            "inline normal must beat author normal at equal importance")) {
        return false;
    }
    if (!require(
            dag_value_for(dag, terminal, "outline") == "2px",
            "inline important must beat author important at equal importance")) {
        return false;
    }
    if (!require(
            dag_value_for(dag, terminal, "border") == "5px",
            "inline-only property must reach the style DAG")) {
        return false;
    }

    return require(
        stats.inline_styles_parsed == 1U &&
            stats.inline_declarations == 5U &&
            stats.inline_parse_work_units > 0U &&
            stats.inline_merge_work_units > 0U &&
            stats.nodes_styled == 1U,
        "inline parse and merge accounting must be published");
}

bool test_inline_style_fail_closed_boundaries() {
    std::pmr::monotonic_buffer_resource stylesheet_memory;
    CssStylesheetV1 sheet(&stylesheet_memory);
    if (!require(
            parse_sheet("div{color:red;}", &sheet),
            "inline failure stylesheet must parse")) {
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
    const std::size_t nodes_before = dag.nodes.size();
    const std::size_t text_before = dag.text.size();

    ZenithSemanticNodeWindowResult mismatch;
    mismatch.start_ordinal = 10U;
    mismatch.next_ordinal = 11U;
    mismatch.arena_node_count = 100U;
    mismatch.nodes.push_back(
        make_node(
            "div",
            {{"style", "color:blue"}},
            "color:red"));
    mismatch.attribute_count = 1U;
    if (!require(
            !compute_css_style_terminals_for_semantic_window_v1(
                sheet,
                mismatch,
                CssSemanticStyleBridgeConfigV1{},
                &dag,
                &output,
                &stats,
                &error) &&
                error.kind ==
                    CssSemanticStyleBridgeErrorKindV1::InvalidWindow,
            "semantic style field mismatch must fail in preflight")) {
        return false;
    }

    constexpr std::string_view malformed_style =
        "color:\"unterminated";
    ZenithSemanticNodeWindowResult malformed;
    malformed.start_ordinal = 20U;
    malformed.next_ordinal = 21U;
    malformed.arena_node_count = 100U;
    malformed.nodes.push_back(
        make_node(
            "div",
            {{"style", std::string(malformed_style)}},
            std::string(malformed_style)));
    malformed.attribute_count = 1U;
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
                    CssSemanticStyleBridgeErrorKindV1::InlineStyleParseFailure &&
                error.parser_kind ==
                    CssParserV1ErrorKind::UnterminatedString,
            "fatal inline declaration-list syntax must expose parser error kind")) {
        return false;
    }

    constexpr std::string_view at_rule_style =
        "@foo x;color:green";
    ZenithSemanticNodeWindowResult at_rule;
    at_rule.start_ordinal = 30U;
    at_rule.next_ordinal = 31U;
    at_rule.arena_node_count = 100U;
    at_rule.nodes.push_back(
        make_node(
            "div",
            {{"style", std::string(at_rule_style)}},
            std::string(at_rule_style)));
    at_rule.attribute_count = 1U;
    if (!require(
            !compute_css_style_terminals_for_semantic_window_v1(
                sheet,
                at_rule,
                CssSemanticStyleBridgeConfigV1{},
                &dag,
                &output,
                &stats,
                &error) &&
                error.kind ==
                    CssSemanticStyleBridgeErrorKindV1::InlineStyleAtRuleUnsupported,
            "style-attribute at-rules must fail closed at bridge boundary")) {
        return false;
    }

    constexpr std::string_view merge_limited_style =
        "color:blue;margin:2px";
    ZenithSemanticNodeWindowResult merge_limited;
    merge_limited.start_ordinal = 40U;
    merge_limited.next_ordinal = 41U;
    merge_limited.arena_node_count = 100U;
    merge_limited.nodes.push_back(
        make_node(
            "div",
            {{"style", std::string(merge_limited_style)}},
            std::string(merge_limited_style)));
    merge_limited.attribute_count = 1U;

    CssSemanticStyleBridgeConfigV1 merge_limited_config;
    merge_limited_config.inline_merge.maximum_inline_declarations = 1U;
    if (!require(
            !compute_css_style_terminals_for_semantic_window_v1(
                sheet,
                merge_limited,
                merge_limited_config,
                &dag,
                &output,
                &stats,
                &error) &&
                error.kind ==
                    CssSemanticStyleBridgeErrorKindV1::InlineCascadeMergeFailure &&
                error.inline_merge_kind ==
                    CssInlineCascadeMergeErrorKindV1::InlineDeclarationLimitExceeded &&
                stats.inline_styles_parsed == 1U &&
                stats.inline_declarations == 2U,
            "inline merge hard-limit failure must preserve nested error kind")) {
        return false;
    }

    return require(
        dag.nodes.size() == nodes_before &&
            dag.text.size() == text_before &&
            output.document_begin == 900U &&
            output.document_end == 901U &&
            output.document_node_count == 1000U &&
            output.terminal_nodes.size() == 1U &&
            output.terminal_nodes[0] == 777U,
        "inline pre-DAG failures must preserve DAG and prior published window");
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
        !test_inline_style_precedence_and_dag_integration() ||
        !test_inline_style_fail_closed_boundaries() ||
        !test_window_and_shared_work_bounds_fail_closed()) {
        return 1;
    }

    std::cout
        << "Z3 semantic style bridge v1 tests passed\n";
    return 0;
}
