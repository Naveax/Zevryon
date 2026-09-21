#include "css_style_materialization_window_v1.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <string_view>
#include <vector>

namespace {

using namespace zevryon::style;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool build_terminal(
    std::string_view source,
    CssComputedStyleDagV1* dag,
    std::uint32_t* terminal) {
    std::pmr::monotonic_buffer_resource scratch;
    CssStylesheetV1 sheet(&scratch);
    CssCascadeResultV1 cascade(&scratch);
    CssParserV1Stats parser_stats;
    CssParserV1Error parser_error;
    CssCascadeStatsV1 cascade_stats;
    CssCascadeErrorV1 cascade_error;
    CssStyleDagStatsV1 dag_stats;
    CssStyleDagErrorV1 dag_error;

    if (!parse_css_stylesheet_v1(
            source,
            CssParserV1Config{},
            &sheet,
            &parser_stats,
            &parser_error)) {
        return false;
    }
    if (!cascade_css_author_rules_v1(
            sheet,
            CssSelectorNodeV1{"div", {}},
            CssCascadeConfigV1{},
            &cascade,
            &cascade_stats,
            &cascade_error)) {
        return false;
    }
    return intern_css_cascade_style_v1(
        sheet,
        cascade,
        CssStyleDagConfigV1{},
        dag,
        terminal,
        &dag_stats,
        &dag_error);
}

struct Fixture {
    Fixture()
        : dag(&memory),
          output(&memory) {}

    std::pmr::monotonic_buffer_resource memory;
    CssComputedStyleDagV1 dag;
    CssStyleMaterializationBatchV1 output;
    std::array<std::uint32_t, 3> terminals{};
};

bool build_fixture(Fixture* fixture) {
    if (fixture == nullptr) {
        return false;
    }
    return build_terminal(
               "div{a:1;}",
               &fixture->dag,
               &fixture->terminals[0]) &&
        build_terminal(
               "div{a:1;b:2;}",
               &fixture->dag,
               &fixture->terminals[1]) &&
        build_terminal(
               "div{a:1;c:3;}",
               &fixture->dag,
               &fixture->terminals[2]);
}

bool materialize_window(
    const Fixture& fixture,
    std::span<const std::uint32_t> document,
    std::size_t visible_begin,
    std::size_t visible_end,
    CssStyleMaterializationWindowConfigV1 config,
    CssStyleMaterializationBatchV1* output,
    CssStyleMaterializationWindowStatsV1* stats,
    CssStyleMaterializationWindowErrorV1* error) {
    return materialize_css_style_window_v1(
        fixture.dag,
        document,
        visible_begin,
        visible_end,
        config,
        output,
        stats,
        error);
}

bool test_bounded_window_selection_and_mapping() {
    Fixture fixture;
    bool ok = expect(
        build_fixture(&fixture),
        "window fixture must build");

    const std::array<std::uint32_t, 10> document{{
        fixture.terminals[0],
        fixture.terminals[1],
        fixture.terminals[2],
        fixture.terminals[0],
        fixture.terminals[1],
        fixture.terminals[2],
        fixture.terminals[0],
        fixture.terminals[1],
        fixture.terminals[2],
        fixture.terminals[0],
    }};

    CssStyleMaterializationWindowConfigV1 config;
    config.maximum_visible_nodes = 4U;
    config.offscreen_before_nodes = 2U;
    config.offscreen_after_nodes = 3U;
    config.maximum_selection_work_units = 16U;

    CssStyleMaterializationWindowStatsV1 stats;
    CssStyleMaterializationWindowErrorV1 error;
    ok &= expect(
        materialize_window(
            fixture,
            document,
            4U,
            6U,
            config,
            &fixture.output,
            &stats,
            &error),
        "bounded visible window must materialize");
    ok &= expect(error.kind ==
                     CssStyleMaterializationWindowErrorKindV1::None,
                 "successful window materialization must clear error");
    ok &= expect(
        stats.document_nodes == 10U &&
            stats.visible_nodes == 2U &&
            stats.offscreen_before_nodes == 2U &&
            stats.offscreen_after_nodes == 3U &&
            stats.selected_requests == 7U &&
            stats.selection_work_units == 7U,
        "window selection stats must be exact");
    ok &= expect(
        stats.selected_document_begin == 2U &&
            stats.selected_document_end == 9U,
        "selected document range must be [2,9)");
    ok &= expect(
        fixture.output.request_style_indices.size() == 7U,
        "each selected document node must receive request mapping");

    for (std::size_t offset = 0U; offset < 7U; ++offset) {
        const std::uint32_t style_index =
            fixture.output.request_style_indices[offset];
        ok &= expect(
            static_cast<std::size_t>(style_index) <
                fixture.output.styles.size(),
            "request mapping must point at retained materialized style");
        if (static_cast<std::size_t>(style_index) <
            fixture.output.styles.size()) {
            ok &= expect(
                fixture.output.styles[
                    static_cast<std::size_t>(style_index)]
                        .terminal_node ==
                    document[2U + offset],
                "request mapping must preserve document-order terminal identity");
        }
    }
    return ok;
}

bool test_large_document_selection_does_not_scan_document() {
    Fixture fixture;
    bool ok = expect(
        build_fixture(&fixture),
        "large-document window fixture must build");

    constexpr std::size_t kDocumentNodes = 100'000U;
    std::vector<std::uint32_t> document(kDocumentNodes);
    for (std::size_t index = 0U;
         index < document.size();
         ++index) {
        document[index] =
            fixture.terminals[index % fixture.terminals.size()];
    }

    CssStyleMaterializationWindowConfigV1 config;
    config.maximum_visible_nodes = 1U;
    config.offscreen_before_nodes = 2U;
    config.offscreen_after_nodes = 2U;
    config.maximum_selection_work_units = 5U;

    CssStyleMaterializationWindowStatsV1 stats;
    CssStyleMaterializationWindowErrorV1 error;
    ok &= expect(
        materialize_window(
            fixture,
            document,
            50'000U,
            50'001U,
            config,
            &fixture.output,
            &stats,
            &error),
        "five-request window inside 100k document must materialize");
    ok &= expect(
        stats.document_nodes == kDocumentNodes,
        "large document count must be reported");
    ok &= expect(
        stats.selected_document_begin == 49'998U &&
            stats.selected_document_end == 50'003U &&
            stats.selected_requests == 5U &&
            stats.selection_work_units == 5U,
        "selection work must depend on five-node window, not document size");
    return ok;
}

bool test_window_clips_at_document_edges() {
    Fixture fixture;
    bool ok = expect(
        build_fixture(&fixture),
        "edge window fixture must build");
    const std::array<std::uint32_t, 10> document{{
        fixture.terminals[0],
        fixture.terminals[1],
        fixture.terminals[2],
        fixture.terminals[0],
        fixture.terminals[1],
        fixture.terminals[2],
        fixture.terminals[0],
        fixture.terminals[1],
        fixture.terminals[2],
        fixture.terminals[0],
    }};

    CssStyleMaterializationWindowConfigV1 config;
    config.maximum_visible_nodes = 2U;
    config.offscreen_before_nodes = 3U;
    config.offscreen_after_nodes = 3U;
    config.maximum_selection_work_units = 8U;

    CssStyleMaterializationWindowStatsV1 stats;
    CssStyleMaterializationWindowErrorV1 error;
    ok &= expect(
        materialize_window(
            fixture,
            document,
            0U,
            1U,
            config,
            &fixture.output,
            &stats,
            &error),
        "leading-edge window must materialize");
    ok &= expect(
        stats.offscreen_before_nodes == 0U &&
            stats.offscreen_after_nodes == 3U &&
            stats.selected_document_begin == 0U &&
            stats.selected_document_end == 4U,
        "leading-edge lookahead must clip without underflow");

    ok &= expect(
        materialize_window(
            fixture,
            document,
            9U,
            10U,
            config,
            &fixture.output,
            &stats,
            &error),
        "trailing-edge window must materialize");
    ok &= expect(
        stats.offscreen_before_nodes == 3U &&
            stats.offscreen_after_nodes == 0U &&
            stats.selected_document_begin == 6U &&
            stats.selected_document_end == 10U,
        "trailing-edge lookahead must clip without overflow");
    return ok;
}

bool test_wrapper_failures_preserve_output() {
    Fixture fixture;
    bool ok = expect(
        build_fixture(&fixture),
        "atomic window fixture must build");
    const std::array<std::uint32_t, 5> document{{
        fixture.terminals[0],
        fixture.terminals[1],
        fixture.terminals[2],
        fixture.terminals[0],
        fixture.terminals[1],
    }};

    CssStyleMaterializationWindowConfigV1 config;
    config.maximum_visible_nodes = 2U;
    config.offscreen_before_nodes = 1U;
    config.offscreen_after_nodes = 1U;
    config.maximum_selection_work_units = 4U;

    CssStyleMaterializationWindowStatsV1 stats;
    CssStyleMaterializationWindowErrorV1 error;
    ok &= expect(
        materialize_window(
            fixture,
            document,
            2U,
            3U,
            config,
            &fixture.output,
            &stats,
            &error),
        "atomic baseline window must materialize");
    const auto styles_before = fixture.output.styles.size();
    const auto properties_before = fixture.output.properties.size();
    const auto text_before = fixture.output.text.size();
    const auto requests_before =
        fixture.output.request_style_indices.size();

    ok &= expect(
        !materialize_window(
            fixture,
            document,
            4U,
            3U,
            config,
            &fixture.output,
            &stats,
            &error),
        "reversed visible range must fail closed");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationWindowErrorKindV1::InvalidVisibleRange,
        "reversed visible range must report exact error");

    config.maximum_visible_nodes = 1U;
    ok &= expect(
        !materialize_window(
            fixture,
            document,
            1U,
            3U,
            config,
            &fixture.output,
            &stats,
            &error),
        "visible-node limit must fail closed");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationWindowErrorKindV1::VisibleNodeLimitExceeded,
        "visible-node limit must report exact error");

    config.maximum_visible_nodes = 2U;
    config.maximum_selection_work_units = 2U;
    ok &= expect(
        !materialize_window(
            fixture,
            document,
            2U,
            3U,
            config,
            &fixture.output,
            &stats,
            &error),
        "selection work budget must fail closed");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationWindowErrorKindV1::SelectionWorkBudgetExceeded,
        "selection work budget must report exact error");

    ok &= expect(
        fixture.output.styles.size() == styles_before &&
            fixture.output.properties.size() == properties_before &&
            fixture.output.text.size() == text_before &&
            fixture.output.request_style_indices.size() ==
                requests_before,
        "wrapper failures must preserve prior materialized output");
    return ok;
}

bool test_nested_materialization_failure_is_mapped() {
    Fixture fixture;
    bool ok = expect(
        build_fixture(&fixture),
        "nested-failure fixture must build");
    const std::array<std::uint32_t, 4> valid_document{{
        fixture.terminals[0],
        fixture.terminals[1],
        fixture.terminals[2],
        fixture.terminals[0],
    }};

    CssStyleMaterializationWindowConfigV1 config;
    config.maximum_visible_nodes = 1U;
    config.offscreen_before_nodes = 0U;
    config.offscreen_after_nodes = 0U;
    config.maximum_selection_work_units = 1U;

    CssStyleMaterializationWindowStatsV1 stats;
    CssStyleMaterializationWindowErrorV1 error;
    ok &= expect(
        materialize_window(
            fixture,
            valid_document,
            0U,
            1U,
            config,
            &fixture.output,
            &stats,
            &error),
        "nested-failure baseline must materialize");
    const auto styles_before = fixture.output.styles.size();
    const auto properties_before = fixture.output.properties.size();
    const auto text_before = fixture.output.text.size();

    const std::array<std::uint32_t, 4> invalid_document{{
        fixture.terminals[0],
        fixture.terminals[1],
        std::numeric_limits<std::uint32_t>::max(),
        fixture.terminals[2],
    }};
    ok &= expect(
        !materialize_window(
            fixture,
            invalid_document,
            2U,
            3U,
            config,
            &fixture.output,
            &stats,
            &error),
        "invalid selected terminal must fail through nested materializer");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationWindowErrorKindV1::MaterializationFailure &&
            error.materialization_kind ==
                CssStyleMaterializationErrorKindV1::InvalidTerminalNode &&
            error.document_index == 2U,
        "nested terminal failure must map exact document index and kind");
    ok &= expect(
        fixture.output.styles.size() == styles_before &&
            fixture.output.properties.size() == properties_before &&
            fixture.output.text.size() == text_before,
        "nested materialization failure must preserve prior output atomically");
    return ok;
}

bool test_invalid_configuration_is_rejected() {
    Fixture fixture;
    bool ok = expect(
        build_fixture(&fixture),
        "invalid-config fixture must build");
    const std::array<std::uint32_t, 1> document{{
        fixture.terminals[0],
    }};

    CssStyleMaterializationWindowConfigV1 config;
    config.maximum_visible_nodes = 4U;
    config.offscreen_before_nodes = 4U;
    config.offscreen_after_nodes = 4U;
    config.materialization.maximum_requests = 8U;

    CssStyleMaterializationWindowStatsV1 stats;
    CssStyleMaterializationWindowErrorV1 error;
    ok &= expect(
        !materialize_window(
            fixture,
            document,
            0U,
            1U,
            config,
            &fixture.output,
            &stats,
            &error),
        "window maxima exceeding nested request capacity must be invalid");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationWindowErrorKindV1::InvalidConfiguration,
        "invalid nested capacity relation must report invalid-configuration");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_bounded_window_selection_and_mapping();
    ok &= test_large_document_selection_does_not_scan_document();
    ok &= test_window_clips_at_document_edges();
    ok &= test_wrapper_failures_preserve_output();
    ok &= test_nested_materialization_failure_is_mapped();
    ok &= test_invalid_configuration_is_rejected();
    if (!ok) {
        return 1;
    }
    std::cout << "CSS style materialization window v1 tests passed\n";
    return 0;
}
