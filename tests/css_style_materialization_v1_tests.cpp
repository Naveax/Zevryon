#include "css_style_materialization_v1.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
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

bool intern_style(
    const CssStylesheetV1& sheet,
    const CssCascadeResultV1& cascade,
    CssComputedStyleDagV1* dag,
    std::uint32_t* terminal) {
    CssStyleDagStatsV1 stats;
    CssStyleDagErrorV1 error;
    return intern_css_cascade_style_v1(
        sheet,
        cascade,
        CssStyleDagConfigV1{},
        dag,
        terminal,
        &stats,
        &error);
}

bool materialize(
    const CssComputedStyleDagV1& dag,
    std::span<const std::uint32_t> terminals,
    CssStyleMaterializationBatchV1* output,
    CssStyleMaterializationStatsV1* stats,
    CssStyleMaterializationErrorV1* error,
    CssStyleMaterializationConfigV1 config = {}) {
    return materialize_css_style_nodes_v1(
        dag,
        terminals,
        config,
        output,
        stats,
        error);
}

struct DagFixture {
    explicit DagFixture(std::pmr::memory_resource* memory)
        : first_sheet(memory),
          second_sheet(memory),
          first_cascade(memory),
          second_cascade(memory),
          dag(memory) {}

    CssStylesheetV1 first_sheet;
    CssStylesheetV1 second_sheet;
    CssCascadeResultV1 first_cascade;
    CssCascadeResultV1 second_cascade;
    CssComputedStyleDagV1 dag;
    std::uint32_t first_terminal{kCssStyleDagNoNodeV1};
    std::uint32_t second_terminal{kCssStyleDagNoNodeV1};
};

bool build_fixture(DagFixture* fixture) {
    if (fixture == nullptr) {
        return false;
    }
    const CssSelectorNodeV1 node{"div", {}};
    return parse_sheet(
               "div{color:green;width:2px;}",
               &fixture->first_sheet) &&
        parse_sheet(
               "div{color:green;z-index:3;}",
               &fixture->second_sheet) &&
        cascade_sheet(
               fixture->first_sheet,
               node,
               &fixture->first_cascade) &&
        cascade_sheet(
               fixture->second_sheet,
               node,
               &fixture->second_cascade) &&
        intern_style(
               fixture->first_sheet,
               fixture->first_cascade,
               &fixture->dag,
               &fixture->first_terminal) &&
        intern_style(
               fixture->second_sheet,
               fixture->second_cascade,
               &fixture->dag,
               &fixture->second_terminal);
}

std::string_view property_name(
    const CssStyleMaterializationBatchV1& batch,
    const CssMaterializedStyleV1& style,
    std::size_t relative) {
    const std::size_t index =
        static_cast<std::size_t>(style.property_offset) + relative;
    return batch.resolve(batch.properties[index].property);
}

std::string_view property_value(
    const CssStyleMaterializationBatchV1& batch,
    const CssMaterializedStyleV1& style,
    std::size_t relative) {
    const std::size_t index =
        static_cast<std::size_t>(style.property_offset) + relative;
    return batch.resolve(batch.properties[index].value);
}

bool test_bounded_batch_and_duplicate_requests() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    DagFixture fixture(&memory);
    bool ok = expect(build_fixture(&fixture), "materialization DAG fixture must build");

    CssStyleMaterializationBatchV1 output(&memory);
    CssStyleMaterializationStatsV1 stats;
    CssStyleMaterializationErrorV1 error;
    const std::array<std::uint32_t, 4> requests{{
        fixture.first_terminal,
        fixture.second_terminal,
        fixture.first_terminal,
        0U,
    }};

    ok &= expect(
        materialize(
            fixture.dag,
            requests,
            &output,
            &stats,
            &error),
        "bounded style batch must materialize");
    ok &= expect(error.kind == CssStyleMaterializationErrorKindV1::None,
                 "successful materialization must clear error");
    ok &= expect(output.request_style_indices.size() == 4U,
                 "all requests must receive style mapping");
    ok &= expect(output.styles.size() == 3U,
                 "duplicate terminal must materialize only once");
    ok &= expect(output.properties.size() == 4U,
                 "two two-property styles plus root must emit four properties");
    ok &= expect(stats.requests == 4U &&
                     stats.unique_styles == 3U &&
                     stats.duplicate_style_requests == 1U,
                 "batch request stats must be exact");
    ok &= expect(
        output.request_style_indices[0] ==
            output.request_style_indices[2],
        "duplicate terminal requests must map to same materialized style");
    ok &= expect(
        output.styles[output.request_style_indices[3]].property_count == 0U,
        "root style must materialize as empty property range");

    const CssMaterializedStyleV1& first =
        output.styles[output.request_style_indices[0]];
    ok &= expect(first.property_count == 2U,
                 "first style must contain two properties");
    ok &= expect(property_name(output, first, 0U) == "color" &&
                     property_value(output, first, 0U) == "green" &&
                     property_name(output, first, 1U) == "width" &&
                     property_value(output, first, 1U) == "2px",
                 "materialized properties must preserve canonical DAG order");
    return ok;
}

bool test_output_owns_text_after_dag_release() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    DagFixture fixture(&memory);
    bool ok = expect(build_fixture(&fixture), "owned-text DAG fixture must build");

    CssStyleMaterializationBatchV1 output(&memory);
    CssStyleMaterializationStatsV1 stats;
    CssStyleMaterializationErrorV1 error;
    const std::array<std::uint32_t, 1> requests{{
        fixture.first_terminal,
    }};
    ok &= expect(
        materialize(
            fixture.dag,
            requests,
            &output,
            &stats,
            &error),
        "owned-text style must materialize");

    fixture.dag.release();
    const CssMaterializedStyleV1& style = output.styles.front();
    ok &= expect(
        property_name(output, style, 0U) == "color" &&
            property_value(output, style, 0U) == "green",
        "materialized output must own text after source DAG release");
    return ok;
}

bool test_atomic_failure_and_invalid_terminal() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    DagFixture fixture(&memory);
    bool ok = expect(build_fixture(&fixture), "atomic materialization fixture must build");

    CssStyleMaterializationBatchV1 output(&memory);
    CssStyleMaterializationStatsV1 stats;
    CssStyleMaterializationErrorV1 error;
    const std::array<std::uint32_t, 1> baseline{{
        fixture.first_terminal,
    }};
    ok &= expect(
        materialize(
            fixture.dag,
            baseline,
            &output,
            &stats,
            &error),
        "atomic materialization baseline must succeed");
    const auto styles_before = output.styles.size();
    const auto properties_before = output.properties.size();
    const auto text_before = output.text.size();

    const std::array<std::uint32_t, 1> invalid{{
        std::numeric_limits<std::uint32_t>::max(),
    }};
    ok &= expect(
        !materialize(
            fixture.dag,
            invalid,
            &output,
            &stats,
            &error),
        "invalid terminal must fail closed");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationErrorKindV1::InvalidTerminalNode,
        "invalid terminal must report exact error");
    ok &= expect(
        output.styles.size() == styles_before &&
            output.properties.size() == properties_before &&
            output.text.size() == text_before,
        "failed materialization must preserve prior output atomically");
    return ok;
}

bool test_corrupt_dag_fails_closed() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    DagFixture fixture(&memory);
    bool ok = expect(build_fixture(&fixture), "corruption DAG fixture must build");

    CssStyleMaterializationBatchV1 output(&memory);
    CssStyleMaterializationStatsV1 stats;
    CssStyleMaterializationErrorV1 error;
    const std::array<std::uint32_t, 1> request{{
        fixture.first_terminal,
    }};

    const std::uint32_t parent =
        fixture.dag.nodes[fixture.first_terminal].parent;
    const CssStyleDagNodeV1 saved =
        fixture.dag.nodes[fixture.first_terminal];
    fixture.dag.nodes[fixture.first_terminal].parent =
        fixture.first_terminal;
    ok &= expect(
        !materialize(
            fixture.dag,
            request,
            &output,
            &stats,
            &error),
        "self-parent DAG node must fail materialization");
    ok &= expect(
        error.kind == CssStyleMaterializationErrorKindV1::CorruptDag,
        "self-parent node must report corrupt-dag");
    fixture.dag.nodes[fixture.first_terminal] = saved;

    const CssTextSliceV1 saved_property =
        fixture.dag.nodes[fixture.first_terminal].property;
    fixture.dag.nodes[fixture.first_terminal].property =
        fixture.dag.nodes[parent].property;
    ok &= expect(
        !materialize(
            fixture.dag,
            request,
            &output,
            &stats,
            &error),
        "non-canonical DAG property order must fail materialization");
    ok &= expect(
        error.kind == CssStyleMaterializationErrorKindV1::CorruptDag,
        "non-canonical property order must report corrupt-dag");
    fixture.dag.nodes[fixture.first_terminal].property = saved_property;
    return ok;
}

bool test_materialization_limits() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    DagFixture fixture(&memory);
    bool ok = expect(build_fixture(&fixture), "limit DAG fixture must build");

    CssStyleMaterializationBatchV1 output(&memory);
    CssStyleMaterializationStatsV1 stats;
    CssStyleMaterializationErrorV1 error;
    const std::array<std::uint32_t, 2> two{{
        fixture.first_terminal,
        fixture.second_terminal,
    }};
    const std::array<std::uint32_t, 1> one{{
        fixture.first_terminal,
    }};

    CssStyleMaterializationConfigV1 config;
    config.maximum_requests = 1U;
    ok &= expect(
        !materialize(
            fixture.dag,
            two,
            &output,
            &stats,
            &error,
            config),
        "request count limit must fail closed");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationErrorKindV1::RequestLimitExceeded,
        "request count limit must report exact error");

    config = CssStyleMaterializationConfigV1{};
    config.maximum_unique_styles = 1U;
    ok &= expect(
        !materialize(
            fixture.dag,
            two,
            &output,
            &stats,
            &error,
            config),
        "unique-style limit must fail closed");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationErrorKindV1::UniqueStyleLimitExceeded,
        "unique-style limit must report exact error");

    config = CssStyleMaterializationConfigV1{};
    config.maximum_properties = 1U;
    ok &= expect(
        !materialize(
            fixture.dag,
            one,
            &output,
            &stats,
            &error,
            config),
        "total property limit must fail closed");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationErrorKindV1::PropertyLimitExceeded,
        "total property limit must report exact error");

    config = CssStyleMaterializationConfigV1{};
    config.maximum_properties_per_style = 1U;
    ok &= expect(
        !materialize(
            fixture.dag,
            one,
            &output,
            &stats,
            &error,
            config),
        "per-style property limit must fail closed");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationErrorKindV1::PropertiesPerStyleExceeded,
        "per-style property limit must report exact error");

    config = CssStyleMaterializationConfigV1{};
    config.maximum_text_bytes = 4U;
    ok &= expect(
        !materialize(
            fixture.dag,
            one,
            &output,
            &stats,
            &error,
            config),
        "output text limit must fail closed");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationErrorKindV1::TextBudgetExceeded,
        "output text limit must report exact error");

    config = CssStyleMaterializationConfigV1{};
    config.maximum_work_units = 1U;
    ok &= expect(
        !materialize(
            fixture.dag,
            one,
            &output,
            &stats,
            &error,
            config),
        "materialization work budget must fail closed");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationErrorKindV1::WorkBudgetExceeded,
        "materialization work budget must report exact error");
    return ok;
}

bool test_real_ledger_rejection() {
    std::pmr::monotonic_buffer_resource fixture_memory;
    DagFixture fixture(&fixture_memory);
    bool ok = expect(build_fixture(&fixture), "ledger DAG fixture must build");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssStyleMaterializationBatchV1 output(&memory);
    CssStyleMaterializationStatsV1 stats;
    CssStyleMaterializationErrorV1 error;
    const std::array<std::uint32_t, 1> request{{
        fixture.first_terminal,
    }};

    ok &= expect(
        !materialize(
            fixture.dag,
            request,
            &output,
            &stats,
            &error),
        "real materialization ledger rejection must fail");
    ok &= expect(
        error.kind ==
            CssStyleMaterializationErrorKindV1::AllocationFailure,
        "real materialization ledger rejection must report allocation-failure");
    ok &= expect(
        ledger.snapshot(ResourceClass::ComputedStyle)
                .rejected_reservations > 0U,
        "materialization ledger rejection must be visible");
    ok &= expect(
        ledger.accounting_clean(),
        "materialization ledger rejection accounting must remain clean");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_bounded_batch_and_duplicate_requests();
    ok &= test_output_owns_text_after_dag_release();
    ok &= test_atomic_failure_and_invalid_terminal();
    ok &= test_corrupt_dag_fails_closed();
    ok &= test_materialization_limits();
    ok &= test_real_ledger_rejection();
    if (!ok) {
        return 1;
    }
    std::cout << "CSS style materialization v1 tests passed\n";
    return 0;
}
