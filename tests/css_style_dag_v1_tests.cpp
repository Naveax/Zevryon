#include "css_style_dag_v1.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <string_view>

namespace {

using zevryon::core::LedgerMemoryResource;
using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;
using namespace zevryon::style;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool parse_sheet(std::string_view source, CssStylesheetV1* sheet) {
    CssParserV1Stats stats;
    CssParserV1Error error;
    return parse_css_stylesheet_v1(
        source,
        CssParserV1Config{},
        sheet,
        &stats,
        &error);
}

bool cascade_sheet(
    const CssStylesheetV1& sheet,
    const CssSelectorNodeV1& node,
    CssCascadeResultV1* result) {
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    return cascade_css_author_rules_v1(
        sheet,
        node,
        CssCascadeConfigV1{},
        result,
        &stats,
        &error);
}

bool intern(
    const CssStylesheetV1& sheet,
    const CssCascadeResultV1& cascade,
    CssComputedStyleDagV1* dag,
    std::uint32_t* terminal,
    CssStyleDagStatsV1* stats,
    CssStyleDagErrorV1* error,
    CssStyleDagConfigV1 config = {}) {
    return intern_css_cascade_style_v1(
        sheet,
        cascade,
        config,
        dag,
        terminal,
        stats,
        error);
}

bool test_exact_style_reuse_across_provenance() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 first_sheet(&sheet_memory);
    CssStylesheetV1 second_sheet(&sheet_memory);
    CssCascadeResultV1 first_cascade(&sheet_memory);
    CssCascadeResultV1 second_cascade(&sheet_memory);

    bool ok = expect(
        parse_sheet(
            "#hero{color:green;width:2px;}",
            &first_sheet),
        "first style sheet must parse");
    ok &= expect(
        parse_sheet(
            "div{width:2px;color:red;} .card{color:green!important;}",
            &second_sheet),
        "second style sheet must parse");

    const std::array<CssSelectorAttributeV1, 2> attributes{{
        {"id", "hero"},
        {"class", "card"},
    }};
    const CssSelectorNodeV1 node{"div", attributes};
    ok &= expect(
        cascade_sheet(first_sheet, node, &first_cascade),
        "first style cascade must succeed");
    ok &= expect(
        cascade_sheet(second_sheet, node, &second_cascade),
        "second style cascade must succeed");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssComputedStyleDagV1 dag(&memory);
    CssStyleDagStatsV1 stats;
    CssStyleDagErrorV1 error;

    std::uint32_t first_terminal = kCssStyleDagNoNodeV1;
    ok &= expect(
        intern(
            first_sheet,
            first_cascade,
            &dag,
            &first_terminal,
            &stats,
            &error),
        "first computed style must intern");
    ok &= expect(first_terminal != kCssStyleDagNoNodeV1,
                 "first computed style must return terminal node");
    ok &= expect(dag.nodes.size() == 3U,
                 "root plus two properties must create three nodes");
    ok &= expect(dag.nodes[first_terminal].depth == 2U,
                 "two-property style terminal depth must be two");

    const std::uint32_t first_parent =
        dag.nodes[first_terminal].parent;
    ok &= expect(
        dag.resolve(dag.nodes[first_parent].property) == "color",
        "canonical first property must be color");
    ok &= expect(
        dag.resolve(dag.nodes[first_terminal].property) == "width",
        "canonical second property must be width");

    const auto nodes_before = dag.nodes.size();
    const auto text_before = dag.text.size();
    std::uint32_t second_terminal = kCssStyleDagNoNodeV1;
    ok &= expect(
        intern(
            second_sheet,
            second_cascade,
            &dag,
            &second_terminal,
            &stats,
            &error),
        "equivalent computed style must intern");
    ok &= expect(
        second_terminal == first_terminal,
        "equal computed property/value sets must share terminal node");
    ok &= expect(
        dag.nodes.size() == nodes_before &&
            dag.text.size() == text_before,
        "equal computed style must add no retained DAG storage");
    ok &= expect(stats.nodes_reused == 2U,
                 "equivalent two-property style must reuse both prefix nodes");
    ok &= expect(stats.nodes_created == 0U,
                 "equivalent style must create no node");
    return ok;
}

bool test_prefix_sharing_and_empty_root() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 first_sheet(&sheet_memory);
    CssStylesheetV1 second_sheet(&sheet_memory);
    CssStylesheetV1 empty_sheet(&sheet_memory);
    CssCascadeResultV1 first_cascade(&sheet_memory);
    CssCascadeResultV1 second_cascade(&sheet_memory);
    CssCascadeResultV1 empty_cascade(&sheet_memory);

    bool ok = expect(
        parse_sheet("div{color:green;width:2px;}", &first_sheet),
        "first prefix sheet must parse");
    ok &= expect(
        parse_sheet("div{color:green;z-index:3;}", &second_sheet),
        "second prefix sheet must parse");
    ok &= expect(
        parse_sheet("span{color:red;}", &empty_sheet),
        "empty-match sheet must parse");

    const CssSelectorNodeV1 div{"div", {}};
    const CssSelectorNodeV1 p{"p", {}};
    ok &= expect(
        cascade_sheet(first_sheet, div, &first_cascade) &&
            cascade_sheet(second_sheet, div, &second_cascade) &&
            cascade_sheet(empty_sheet, p, &empty_cascade),
        "prefix cascades must succeed");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssComputedStyleDagV1 dag(&memory);
    CssStyleDagStatsV1 stats;
    CssStyleDagErrorV1 error;

    std::uint32_t first_terminal = kCssStyleDagNoNodeV1;
    std::uint32_t second_terminal = kCssStyleDagNoNodeV1;
    std::uint32_t empty_terminal = kCssStyleDagNoNodeV1;
    ok &= expect(
        intern(
            first_sheet,
            first_cascade,
            &dag,
            &first_terminal,
            &stats,
            &error),
        "first prefix style must intern");
    const auto first_nodes = dag.nodes.size();
    ok &= expect(
        intern(
            second_sheet,
            second_cascade,
            &dag,
            &second_terminal,
            &stats,
            &error),
        "second prefix style must intern");
    ok &= expect(first_terminal != second_terminal,
                 "different computed styles need different terminal nodes");
    ok &= expect(stats.nodes_reused == 1U && stats.nodes_created == 1U,
                 "styles sharing color prefix must reuse one and create one node");
    ok &= expect(
        stats.child_lookup_nodes_scanned > 0U &&
            stats.work_units >= stats.child_lookup_nodes_scanned,
        "child lookup node scans must consume the shared work budget");
    ok &= expect(dag.nodes.size() == first_nodes + 1U,
                 "prefix sharing must add only one retained node");

    ok &= expect(
        intern(
            empty_sheet,
            empty_cascade,
            &dag,
            &empty_terminal,
            &stats,
            &error),
        "empty computed style must intern");
    ok &= expect(empty_terminal == 0U,
                 "empty computed style must resolve to root node");
    return ok;
}

bool test_property_order_is_canonical() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 sheet(&sheet_memory);
    CssCascadeResultV1 cascade(&sheet_memory);
    bool ok = expect(
        parse_sheet(
            "div{z-index:3;color:green;background:black;}",
            &sheet),
        "canonical ordering sheet must parse");
    ok &= expect(
        cascade_sheet(sheet, CssSelectorNodeV1{"div", {}}, &cascade),
        "canonical ordering cascade must succeed");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssComputedStyleDagV1 dag(&memory);
    CssStyleDagStatsV1 stats;
    CssStyleDagErrorV1 error;
    std::uint32_t terminal = kCssStyleDagNoNodeV1;
    ok &= expect(
        intern(sheet, cascade, &dag, &terminal, &stats, &error),
        "canonical ordering style must intern");

    std::array<std::string_view, 3> reversed{};
    std::uint32_t cursor = terminal;
    for (std::size_t index = 0U; index < reversed.size(); ++index) {
        reversed[index] = dag.resolve(dag.nodes[cursor].property);
        cursor = dag.nodes[cursor].parent;
    }
    ok &= expect(
        reversed[2] == "background" &&
            reversed[1] == "color" &&
            reversed[0] == "z-index",
        "style chain must use canonical bytewise property order");
    return ok;
}

bool test_invalid_cascade_and_corrupt_dag() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 sheet(&sheet_memory);
    CssCascadeResultV1 cascade(&sheet_memory);
    bool ok = expect(
        parse_sheet("div{color:green;width:2px;}", &sheet),
        "corruption sheet must parse");
    ok &= expect(
        cascade_sheet(sheet, CssSelectorNodeV1{"div", {}}, &cascade),
        "corruption cascade must succeed");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssComputedStyleDagV1 dag(&memory);
    CssStyleDagStatsV1 stats;
    CssStyleDagErrorV1 error;
    std::uint32_t terminal = kCssStyleDagNoNodeV1;

    ok &= expect(
        intern(sheet, cascade, &dag, &terminal, &stats, &error),
        "corruption baseline style must intern");
    const auto nodes_before = dag.nodes.size();
    const auto text_before = dag.text.size();

    CssCascadeResultV1 duplicate(&sheet_memory);
    duplicate.winners.push_back(cascade.winners.front());
    duplicate.winners.push_back(cascade.winners.front());
    terminal = 123U;
    ok &= expect(
        !intern(sheet, duplicate, &dag, &terminal, &stats, &error),
        "duplicate cascade property must fail closed");
    ok &= expect(
        error.kind == CssStyleDagErrorKindV1::InvalidCascadeResult,
        "duplicate cascade property must report invalid-cascade-result");
    ok &= expect(terminal == kCssStyleDagNoNodeV1,
                 "failed intern must leave terminal invalid");
    ok &= expect(
        dag.nodes.size() == nodes_before &&
            dag.text.size() == text_before,
        "failed duplicate-property intern must preserve DAG logical state");

    const CssStyleDagNodeV1 saved = dag.nodes[1U];
    dag.nodes[1U].parent = 1U;
    ok &= expect(
        !intern(sheet, cascade, &dag, &terminal, &stats, &error),
        "corrupt DAG parent topology must fail closed");
    ok &= expect(
        error.kind == CssStyleDagErrorKindV1::CorruptDag,
        "corrupt DAG parent must report corrupt-dag");
    dag.nodes[1U] = saved;

    const CssTextSliceV1 saved_property = dag.nodes[2U].property;
    dag.nodes[2U].property = dag.nodes[1U].property;
    ok &= expect(
        !intern(sheet, cascade, &dag, &terminal, &stats, &error),
        "non-canonical retained property order must fail closed");
    ok &= expect(
        error.kind == CssStyleDagErrorKindV1::CorruptDag,
        "non-canonical retained order must report corrupt-dag");
    dag.nodes[2U].property = saved_property;
    return ok;
}

bool test_bounds_and_rollback() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 baseline_sheet(&sheet_memory);
    CssStylesheetV1 extension_sheet(&sheet_memory);
    CssCascadeResultV1 baseline_cascade(&sheet_memory);
    CssCascadeResultV1 extension_cascade(&sheet_memory);
    bool ok = expect(
        parse_sheet("div{color:green;}", &baseline_sheet),
        "bound baseline sheet must parse");
    ok &= expect(
        parse_sheet(
            "div{color:green;width:2px;z-index:3;}",
            &extension_sheet),
        "bound extension sheet must parse");
    const CssSelectorNodeV1 node{"div", {}};
    ok &= expect(
        cascade_sheet(baseline_sheet, node, &baseline_cascade) &&
            cascade_sheet(extension_sheet, node, &extension_cascade),
        "bound cascades must succeed");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssComputedStyleDagV1 dag(&memory);
    CssStyleDagStatsV1 stats;
    CssStyleDagErrorV1 error;
    std::uint32_t terminal = kCssStyleDagNoNodeV1;
    ok &= expect(
        intern(
            baseline_sheet,
            baseline_cascade,
            &dag,
            &terminal,
            &stats,
            &error),
        "bound baseline style must intern");

    const auto nodes_before = dag.nodes.size();
    const auto text_before = dag.text.size();
    CssStyleDagConfigV1 config;
    config.maximum_nodes = nodes_before + 1U;
    ok &= expect(
        !intern(
            extension_sheet,
            extension_cascade,
            &dag,
            &terminal,
            &stats,
            &error,
            config),
        "node bound must fail after one possible extension node");
    ok &= expect(
        error.kind == CssStyleDagErrorKindV1::NodeLimitExceeded,
        "node bound must report node-limit-exceeded");
    ok &= expect(
        dag.nodes.size() == nodes_before &&
            dag.text.size() == text_before,
        "node-bound failure must roll back logical DAG extension");

    config = CssStyleDagConfigV1{};
    config.maximum_properties_per_style = 2U;
    ok &= expect(
        !intern(
            extension_sheet,
            extension_cascade,
            &dag,
            &terminal,
            &stats,
            &error,
            config),
        "property count limit must fail closed");
    ok &= expect(
        error.kind == CssStyleDagErrorKindV1::PropertyLimitExceeded,
        "property limit must report exact error");

    config = CssStyleDagConfigV1{};
    config.maximum_style_semantic_bytes = 8U;
    ok &= expect(
        !intern(
            extension_sheet,
            extension_cascade,
            &dag,
            &terminal,
            &stats,
            &error,
            config),
        "style semantic-byte limit must fail closed");
    ok &= expect(
        error.kind == CssStyleDagErrorKindV1::SemanticBudgetExceeded,
        "semantic-byte limit must report exact error");

    config = CssStyleDagConfigV1{};
    config.maximum_work_units = 1U;
    ok &= expect(
        !intern(
            extension_sheet,
            extension_cascade,
            &dag,
            &terminal,
            &stats,
            &error,
            config),
        "style DAG work budget must fail closed");
    ok &= expect(
        error.kind == CssStyleDagErrorKindV1::WorkBudgetExceeded,
        "work budget must report exact error");

    ResourceLedger small_ledger;
    small_ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U);
    LedgerMemoryResource small_memory(
        small_ledger, ResourceClass::ComputedStyle);
    CssComputedStyleDagV1 small_dag(&small_memory);
    config = CssStyleDagConfigV1{};
    ok &= expect(
        !intern(
            baseline_sheet,
            baseline_cascade,
            &small_dag,
            &terminal,
            &stats,
            &error,
            config),
        "real style DAG ledger rejection must fail");
    ok &= expect(
        error.kind == CssStyleDagErrorKindV1::AllocationFailure,
        "real style DAG ledger rejection must report allocation-failure");
    ok &= expect(
        small_ledger.snapshot(ResourceClass::ComputedStyle)
                .rejected_reservations > 0U,
        "style DAG ledger rejection must be visible");
    ok &= expect(
        small_ledger.accounting_clean(),
        "style DAG ledger rejection accounting must remain clean");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_exact_style_reuse_across_provenance();
    ok &= test_prefix_sharing_and_empty_root();
    ok &= test_property_order_is_canonical();
    ok &= test_invalid_cascade_and_corrupt_dag();
    ok &= test_bounds_and_rollback();
    if (!ok) {
        return 1;
    }
    std::cout << "CSS computed-style DAG v1 tests passed\n";
    return 0;
}
