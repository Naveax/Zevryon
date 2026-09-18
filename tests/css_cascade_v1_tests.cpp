#include "css_cascade_v1.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <memory_resource>
#include <limits>
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

const CssCascadeWinnerV1* find_winner(
    const CssStylesheetV1& sheet,
    const CssCascadeResultV1& result,
    std::string_view property) {
    for (const CssCascadeWinnerV1& winner : result.winners) {
        if (sheet.resolve(winner.property) == property) {
            return &winner;
        }
    }
    return nullptr;
}

bool cascade(
    const CssStylesheetV1& sheet,
    const CssSelectorNodeV1& node,
    CssCascadeResultV1* result,
    CssCascadeStatsV1* stats,
    CssCascadeErrorV1* error,
    CssCascadeConfigV1 config = {}) {
    return cascade_css_author_rules_v1(
        sheet, node, config, result, stats, error);
}

bool test_wpt_specificity_attribute_over_type() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 sheet(&sheet_memory);
    bool ok = expect(
        parse_sheet(
            "[id=id1]{color:green;} div{color:red;}",
            &sheet),
        "specificity-001 slice must parse");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCascadeResultV1 result(&memory);
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    const std::array<CssSelectorAttributeV1, 1> attributes{{
        {"id", "id1"},
    }};
    const CssSelectorNodeV1 node{"div", attributes};

    ok &= expect(
        cascade(sheet, node, &result, &stats, &error),
        "specificity-001 cascade must succeed");
    const CssCascadeWinnerV1* winner =
        find_winner(sheet, result, "color");
    ok &= expect(winner != nullptr, "color winner must exist");
    if (winner != nullptr) {
        ok &= expect(
            sheet.resolve(winner->value) == "green",
            "attribute selector must outrank later type selector");
        ok &= expect(
            winner->specificity == CssSpecificityV1{0U, 1U, 0U},
            "winning specificity must be attribute specificity");
    }
    return ok;
}

bool test_wpt_specificity_type_over_universal() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 sheet(&sheet_memory);
    bool ok = expect(
        parse_sheet(
            "div{color:green;} *{color:red;} p{color:black;}",
            &sheet),
        "specificity-007 slice must parse");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCascadeResultV1 result(&memory);
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    const CssSelectorNodeV1 node{"div", {}};

    ok &= expect(
        cascade(sheet, node, &result, &stats, &error),
        "specificity-007 cascade must succeed");
    const CssCascadeWinnerV1* winner =
        find_winner(sheet, result, "color");
    ok &= expect(winner != nullptr, "color winner must exist");
    if (winner != nullptr) {
        ok &= expect(
            sheet.resolve(winner->value) == "green",
            "type selector must outrank later universal selector");
    }
    return ok;
}

bool test_wpt_later_source_order() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 sheet(&sheet_memory);
    bool ok = expect(
        parse_sheet(
            "div{color:red;} div{color:green;}",
            &sheet),
        "cascade-005 slice must parse");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCascadeResultV1 result(&memory);
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    const CssSelectorNodeV1 node{"div", {}};

    ok &= expect(
        cascade(sheet, node, &result, &stats, &error),
        "cascade-005 cascade must succeed");
    const CssCascadeWinnerV1* winner =
        find_winner(sheet, result, "color");
    ok &= expect(winner != nullptr, "source-order color winner must exist");
    if (winner != nullptr) {
        ok &= expect(
            sheet.resolve(winner->value) == "green",
            "later declaration must win equal importance and specificity");
    }
    ok &= expect(
        stats.wins_by_source_order == 1U,
        "source-order replacement stats must be exact");
    return ok;
}

bool test_importance_and_specificity_precedence() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 sheet(&sheet_memory);
    bool ok = expect(
        parse_sheet(
            "#id1{color:green;} div{color:red!important;}"
            "div{width:1px;} [id=id1]{width:2px;}",
            &sheet),
        "importance/specificity sheet must parse");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCascadeResultV1 result(&memory);
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    const std::array<CssSelectorAttributeV1, 1> attributes{{
        {"id", "id1"},
    }};
    const CssSelectorNodeV1 node{"div", attributes};

    ok &= expect(
        cascade(sheet, node, &result, &stats, &error),
        "importance/specificity cascade must succeed");
    const CssCascadeWinnerV1* color =
        find_winner(sheet, result, "color");
    const CssCascadeWinnerV1* width =
        find_winner(sheet, result, "width");
    ok &= expect(color != nullptr && width != nullptr,
                 "color and width winners must exist");
    if (color != nullptr) {
        ok &= expect(
            sheet.resolve(color->value) == "red" && color->important,
            "important declaration must outrank higher normal specificity");
    }
    if (width != nullptr) {
        ok &= expect(
            sheet.resolve(width->value) == "2px",
            "higher specificity must win at equal importance");
    }
    ok &= expect(stats.wins_by_importance == 1U,
                 "importance replacement stats must be exact");
    ok &= expect(stats.wins_by_specificity == 1U,
                 "specificity replacement stats must be exact");
    return ok;
}

bool test_unmatched_rules_and_multiple_properties() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 sheet(&sheet_memory);
    bool ok = expect(
        parse_sheet(
            "span{color:red;} div{color:green;width:2px;}",
            &sheet),
        "multi-property sheet must parse");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCascadeResultV1 result(&memory);
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    const CssSelectorNodeV1 node{"div", {}};

    ok &= expect(
        cascade(sheet, node, &result, &stats, &error),
        "unmatched-rule cascade must succeed");
    ok &= expect(stats.rules_considered == 2U && stats.matched_rules == 1U,
                 "matched rule stats must be exact");
    ok &= expect(stats.declarations_considered == 2U,
                 "only matched declarations must be considered");
    ok &= expect(result.winners.size() == 2U,
                 "two matched properties must be emitted");
    return ok;
}

bool test_atomic_failure_and_limits() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 stable_sheet(&sheet_memory);
    CssStylesheetV1 unsupported_sheet(&sheet_memory);
    bool ok = expect(
        parse_sheet("div{color:green;}", &stable_sheet),
        "stable cascade sheet must parse");
    ok &= expect(
        parse_sheet("div > span{color:red;}", &unsupported_sheet),
        "unsupported selector sheet must remain syntactically parseable");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCascadeResultV1 result(&memory);
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    const CssSelectorNodeV1 node{"div", {}};

    ok &= expect(
        cascade(stable_sheet, node, &result, &stats, &error),
        "stable cascade baseline must succeed");
    const auto stable_winners = result.winners.size();

    ok &= expect(
        !cascade(unsupported_sheet, node, &result, &stats, &error),
        "unsupported selector must fail cascade closed");
    ok &= expect(
        error.kind == CssCascadeErrorKindV1::SelectorCompileFailure,
        "unsupported selector must report selector-compile-failure");
    ok &= expect(
        result.winners.size() == stable_winners,
        "failed cascade must preserve previous output atomically");

    CssCascadeConfigV1 config;
    config.maximum_work_units = 1U;
    ok &= expect(
        !cascade(stable_sheet, node, &result, &stats, &error, config),
        "work budget must fail closed");
    ok &= expect(
        error.kind == CssCascadeErrorKindV1::WorkBudgetExceeded,
        "work budget must report exact error kind");

    std::pmr::monotonic_buffer_resource limit_sheet_memory;
    CssStylesheetV1 limit_sheet(&limit_sheet_memory);
    ok &= expect(
        parse_sheet("div{color:green;width:2px;}", &limit_sheet),
        "property-limit sheet must parse");
    config = CssCascadeConfigV1{};
    config.maximum_properties = 1U;
    ok &= expect(
        !cascade(limit_sheet, node, &result, &stats, &error, config),
        "property limit must fail closed");
    ok &= expect(
        error.kind == CssCascadeErrorKindV1::PropertyLimitExceeded,
        "property limit must report exact error kind");
    return ok;
}

bool test_corrupt_stylesheet_records_fail_closed() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 stable_sheet(&sheet_memory);
    CssStylesheetV1 corrupt_sheet(&sheet_memory);
    bool ok = expect(
        parse_sheet("div{color:green;}", &stable_sheet),
        "stable corruption baseline must parse");
    ok &= expect(
        parse_sheet("div{color:red;}", &corrupt_sheet),
        "corruption candidate must parse");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCascadeResultV1 result(&memory);
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    const CssSelectorNodeV1 node{"div", {}};

    ok &= expect(
        cascade(stable_sheet, node, &result, &stats, &error),
        "corruption baseline cascade must succeed");
    const auto stable_winners = result.winners.size();

    const CssStyleRuleV1 original_rule = corrupt_sheet.rules.front();
    corrupt_sheet.rules.front().selector.offset =
        std::numeric_limits<std::uint32_t>::max();
    ok &= expect(
        !cascade(corrupt_sheet, node, &result, &stats, &error),
        "corrupt selector slice must fail closed");
    ok &= expect(
        error.kind == CssCascadeErrorKindV1::InvalidStylesheet,
        "corrupt selector slice must report invalid-stylesheet");
    ok &= expect(
        result.winners.size() == stable_winners,
        "corrupt selector failure must preserve prior output");

    corrupt_sheet.rules.front() = original_rule;
    corrupt_sheet.rules.front().declaration_offset =
        std::numeric_limits<std::uint32_t>::max();
    ok &= expect(
        !cascade(corrupt_sheet, node, &result, &stats, &error),
        "corrupt declaration range must fail closed");
    ok &= expect(
        error.kind == CssCascadeErrorKindV1::InvalidStylesheet,
        "corrupt declaration range must report invalid-stylesheet");

    corrupt_sheet.rules.front() = original_rule;
    const CssDeclarationV1 original_declaration =
        corrupt_sheet.declarations.front();
    corrupt_sheet.declarations.front().property.offset =
        std::numeric_limits<std::uint32_t>::max();
    ok &= expect(
        !cascade(corrupt_sheet, node, &result, &stats, &error),
        "corrupt declaration slice must fail closed");
    ok &= expect(
        error.kind == CssCascadeErrorKindV1::InvalidStylesheet,
        "corrupt declaration slice must report invalid-stylesheet");
    corrupt_sheet.declarations.front() = original_declaration;
    return ok;
}

bool test_aggregate_match_work_budget() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 sheet(&sheet_memory);
    bool ok = expect(
        parse_sheet(
            ".alpha{color:green;} .beta{width:2px;}",
            &sheet),
        "aggregate match-work sheet must parse");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCascadeResultV1 result(&memory);
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    const std::array<CssSelectorAttributeV1, 1> attributes{{
        {"class", "alpha beta"},
    }};
    const CssSelectorNodeV1 node{"div", attributes};

    CssCascadeConfigV1 config;
    config.maximum_work_units = 24U;
    ok &= expect(
        !cascade(sheet, node, &result, &stats, &error, config),
        "aggregate selector-match work budget must fail closed");
    ok &= expect(
        error.kind == CssCascadeErrorKindV1::WorkBudgetExceeded,
        "aggregate match work must report work-budget-exceeded");
    ok &= expect(
        stats.work_units <= config.maximum_work_units,
        "failed aggregate work accounting must remain within budget");
    return ok;
}

bool test_real_ledger_rejection() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 sheet(&sheet_memory);
    bool ok = expect(
        parse_sheet(
            "div{a:1;b:2;c:3;d:4;e:5;f:6;g:7;h:8;}",
            &sheet),
        "ledger rejection sheet must parse");

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCascadeResultV1 result(&memory);
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    const CssSelectorNodeV1 node{"div", {}};

    const bool cascaded =
        cascade(sheet, node, &result, &stats, &error);
    ok &= expect(!cascaded, "real cascade ledger rejection must fail");
    ok &= expect(
        error.kind == CssCascadeErrorKindV1::AllocationFailure,
        "real cascade ledger rejection must report allocation-failure");
    ok &= expect(
        ledger.snapshot(ResourceClass::ComputedStyle).rejected_reservations >
            0U,
        "cascade ledger rejection must be visible in resource ledger");
    ok &= expect(ledger.accounting_clean(),
                 "cascade ledger rejection accounting must remain clean");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_wpt_specificity_attribute_over_type();
    ok &= test_wpt_specificity_type_over_universal();
    ok &= test_wpt_later_source_order();
    ok &= test_importance_and_specificity_precedence();
    ok &= test_unmatched_rules_and_multiple_properties();
    ok &= test_atomic_failure_and_limits();
    ok &= test_corrupt_stylesheet_records_fail_closed();
    ok &= test_aggregate_match_work_budget();
    ok &= test_real_ledger_rejection();
    if (!ok) {
        return 1;
    }
    std::cout << "CSS cascade v1 tests passed\n";
    return 0;
}
