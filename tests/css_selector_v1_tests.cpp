#include "css_selector_v1.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstddef>
#include <iostream>
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

bool compile(
    std::string_view source,
    CssCompoundSelectorV1* selector,
    CssSelectorCompileStatsV1* stats,
    CssSelectorCompileErrorV1* error,
    CssSelectorCompileConfigV1 config = {}) {
    return compile_css_compound_selector_v1(
        source, config, selector, stats, error);
}

bool test_compilation_and_specificity() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorCompileStatsV1 stats;
    CssSelectorCompileErrorV1 error;

    bool ok = expect(
        compile(
            "DIV#hero.card.primary[data-state=\"open\"][hidden]",
            &selector,
            &stats,
            &error),
        "compound selector must compile");
    ok &= expect(error.kind == CssSelectorCompileErrorKindV1::None,
                 "successful selector compile must clear error");
    ok &= expect(selector.simple.size() == 6U,
                 "selector must contain six simple selectors");
    ok &= expect(
        selector.specificity == CssSpecificityV1{1U, 4U, 1U},
        "specificity must count ID, class/attribute and type components");
    ok &= expect(stats.simple_selectors == 6U,
                 "simple-selector stats must be exact");
    ok &= expect(stats.type_selectors == 1U &&
                     stats.id_selectors == 1U &&
                     stats.class_selectors == 2U &&
                     stats.attribute_selectors == 2U,
                 "selector-kind stats must be exact");

    ok &= expect(
        compare_css_specificity_v1(
            CssSpecificityV1{1U, 0U, 0U},
            CssSpecificityV1{0U, 99U, 99U}) > 0,
        "ID specificity must dominate class/type components");
    ok &= expect(
        compare_css_specificity_v1(
            CssSpecificityV1{0U, 2U, 0U},
            CssSpecificityV1{0U, 1U, 99U}) > 0,
        "class specificity must dominate type components");
    ok &= expect(
        compare_css_specificity_v1(
            CssSpecificityV1{0U, 1U, 2U},
            CssSpecificityV1{0U, 1U, 2U}) == 0,
        "equal specificity must compare equal");

    ok &= expect(ledger.accounting_clean(),
                 "selector compile accounting must remain clean");
    ok &= expect(ledger.within_hard_limits(),
                 "selector compile must remain within ledger limit");
    return ok;
}

bool test_matching() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorCompileStatsV1 stats;
    CssSelectorCompileErrorV1 error;

    bool ok = expect(
        compile(
            "DIV#hero.card.primary[DATA-STATE=open][hidden]",
            &selector,
            &stats,
            &error),
        "matching selector must compile");

    const std::array<CssSelectorAttributeV1, 5> attributes{{
        {"id", "hero"},
        {"class", "card secondary primary"},
        {"data-state", "open"},
        {"hidden", ""},
        {"title", "Example"},
    }};
    const CssSelectorNodeV1 node{"div", attributes};

    bool matched = false;
    ok &= expect(
        match_css_compound_selector_v1(selector, node, &matched) && matched,
        "type/id/class/attribute compound selector must match");

    const std::array<CssSelectorAttributeV1, 4> wrong_class{{
        {"id", "hero"},
        {"class", "card secondary"},
        {"data-state", "open"},
        {"hidden", ""},
    }};
    matched = true;
    ok &= expect(
        match_css_compound_selector_v1(
            selector,
            CssSelectorNodeV1{"DIV", wrong_class},
            &matched) &&
            !matched,
        "missing class token must reject match");

    const std::array<CssSelectorAttributeV1, 5> wrong_value{{
        {"id", "hero"},
        {"class", "card primary"},
        {"data-state", "OPEN"},
        {"hidden", ""},
        {"title", "Example"},
    }};
    matched = true;
    ok &= expect(
        match_css_compound_selector_v1(
            selector,
            CssSelectorNodeV1{"div", wrong_value},
            &matched) &&
            !matched,
        "attribute values must remain case-sensitive in foundation");

    ok &= expect(
        !match_css_compound_selector_v1(selector, node, nullptr),
        "null match output must fail closed");
    return ok;
}

bool test_quoted_attribute_values() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorCompileStatsV1 stats;
    CssSelectorCompileErrorV1 error;

    bool ok = expect(
        compile(
            "[ title = \"hello world\"][empty=\"\"][data-x='Mixed Case']",
            &selector,
            &stats,
            &error),
        "quoted attribute values and internal whitespace must compile");
    ok &= expect(
        selector.specificity == CssSpecificityV1{0U, 3U, 0U},
        "three attribute selectors must contribute class specificity");

    const std::array<CssSelectorAttributeV1, 3> attributes{{
        {"TITLE", "hello world"},
        {"empty", ""},
        {"data-x", "Mixed Case"},
    }};
    bool matched = false;
    ok &= expect(
        match_css_compound_selector_v1(
            selector,
            CssSelectorNodeV1{"div", attributes},
            &matched) &&
            matched,
        "quoted and empty exact attribute values must match");

    const std::array<CssSelectorAttributeV1, 3> wrong_case{{
        {"title", "hello world"},
        {"empty", ""},
        {"data-x", "mixed case"},
    }};
    matched = true;
    ok &= expect(
        match_css_compound_selector_v1(
            selector,
            CssSelectorNodeV1{"div", wrong_case},
            &matched) &&
            !matched,
        "quoted attribute values must remain case-sensitive");
    return ok;
}

bool test_universal_and_class_tokens() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorCompileStatsV1 stats;
    CssSelectorCompileErrorV1 error;

    bool ok = expect(
        compile("*.alpha", &selector, &stats, &error),
        "universal compound selector must compile");
    ok &= expect(
        selector.specificity == CssSpecificityV1{0U, 1U, 0U},
        "universal selector must not add specificity");

    const std::array<CssSelectorAttributeV1, 1> attributes{{
        {"class", "beta\talpha\ngamma"},
    }};
    bool matched = false;
    ok &= expect(
        match_css_compound_selector_v1(
            selector,
            CssSelectorNodeV1{"anything", attributes},
            &matched) &&
            matched,
        "class matching must use HTML ASCII-whitespace tokens");
    return ok;
}

bool test_unsupported_and_invalid_syntax() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorCompileStatsV1 stats;
    CssSelectorCompileErrorV1 error;

    bool ok = true;
    for (const std::string_view source : {
             "div > span",
             "div span",
             "div:hover",
             "div,span",
             "[title~=word]",
             ".foo\\bar",
         }) {
        ok &= expect(
            !compile(source, &selector, &stats, &error),
            "unsupported selector syntax must fail closed");
        ok &= expect(
            error.kind == CssSelectorCompileErrorKindV1::UnsupportedSyntax,
            "unsupported selector must report unsupported-syntax");
    }

    ok &= expect(
        !compile("#", &selector, &stats, &error),
        "missing ID identifier must fail");
    ok &= expect(
        error.kind == CssSelectorCompileErrorKindV1::InvalidIdentifier,
        "missing ID identifier must report invalid-identifier");

    for (const std::string_view source : {"-", "-5", ".-5", "#-"}) {
        ok &= expect(
            !compile(source, &selector, &stats, &error),
            "invalid hyphen-start identifier must fail");
        ok &= expect(
            error.kind == CssSelectorCompileErrorKindV1::InvalidIdentifier,
            "invalid hyphen-start identifier must report invalid-identifier");
    }
    ok &= expect(
        compile("-custom", &selector, &stats, &error),
        "hyphen followed by identifier start must compile");
    ok &= expect(
        compile("--custom", &selector, &stats, &error),
        "double-hyphen identifier must compile");

    ok &= expect(
        !compile("[data-x=", &selector, &stats, &error),
        "unterminated attribute selector must fail");
    ok &= expect(
        error.kind == CssSelectorCompileErrorKindV1::InvalidAttribute,
        "unterminated attribute selector must report invalid-attribute");
    return ok;
}

bool test_match_bounds_and_corruption() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorCompileStatsV1 stats;
    CssSelectorCompileErrorV1 error;

    bool ok = expect(
        compile(".alpha[data-x=ok]", &selector, &stats, &error),
        "bounded-match selector must compile");
    const std::array<CssSelectorAttributeV1, 2> attributes{{
        {"class", "alpha"},
        {"data-x", "ok"},
    }};
    const CssSelectorNodeV1 node{"div", attributes};

    bool matched = false;
    ok &= expect(
        match_css_compound_selector_v1(selector, node, &matched) && matched,
        "default selector match budget must admit small semantic node");

    CssSelectorMatchConfigV1 config;
    config.maximum_attributes = 1U;
    matched = true;
    ok &= expect(
        !match_css_compound_selector_v1(
            selector, node, config, &matched) &&
            !matched,
        "attribute-count budget must fail closed");

    config = CssSelectorMatchConfigV1{};
    config.maximum_semantic_bytes = 4U;
    matched = true;
    ok &= expect(
        !match_css_compound_selector_v1(
            selector, node, config, &matched) &&
            !matched,
        "semantic-byte budget must fail closed");

    config = CssSelectorMatchConfigV1{};
    config.maximum_work_units = 1U;
    matched = true;
    ok &= expect(
        !match_css_compound_selector_v1(
            selector, node, config, &matched) &&
            !matched,
        "match work-unit budget must fail closed");

    config = CssSelectorMatchConfigV1{};
    config.maximum_attributes = 0U;
    matched = true;
    ok &= expect(
        !match_css_compound_selector_v1(
            selector, node, config, &matched) &&
            !matched,
        "invalid match configuration must fail closed");

    selector.simple[0].kind =
        static_cast<CssSelectorSimpleKindV1>(0xffU);
    matched = true;
    ok &= expect(
        !match_css_compound_selector_v1(selector, node, &matched) &&
            !matched,
        "unknown selector kind must fail closed");
    return ok;
}

bool test_atomic_failure_and_limits() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorCompileStatsV1 stats;
    CssSelectorCompileErrorV1 error;

    bool ok = expect(
        compile(".stable", &selector, &stats, &error),
        "atomic baseline selector must compile");
    const std::string_view stable_name =
        selector.resolve(selector.simple.front().name);

    ok &= expect(
        !compile("div > span", &selector, &stats, &error),
        "failing compile must reject unsupported combinator");
    ok &= expect(
        selector.simple.size() == 1U &&
            selector.resolve(selector.simple.front().name) == stable_name,
        "failed compile must preserve prior selector atomically");

    CssSelectorCompileConfigV1 config;
    config.maximum_simple_selectors = 2U;
    ok &= expect(
        !compile(".a.b.c", &selector, &stats, &error, config),
        "simple-selector limit must fail closed");
    ok &= expect(
        error.kind ==
            CssSelectorCompileErrorKindV1::SimpleSelectorLimitExceeded,
        "simple-selector limit must report exact error kind");

    config = CssSelectorCompileConfigV1{};
    config.maximum_output_text_bytes = 3U;
    ok &= expect(
        !compile(".abcdef", &selector, &stats, &error, config),
        "output text budget must fail closed");
    ok &= expect(
        error.kind == CssSelectorCompileErrorKindV1::OutputBudgetExceeded,
        "output budget must report exact error kind");

    config = CssSelectorCompileConfigV1{};
    config.maximum_input_bytes = 3U;
    ok &= expect(
        !compile(".abcd", &selector, &stats, &error, config),
        "input byte budget must fail closed");
    ok &= expect(
        error.kind == CssSelectorCompileErrorKindV1::InputTooLarge,
        "input budget must report exact error kind");
    return ok;
}

bool test_real_ledger_rejection() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorCompileStatsV1 stats;
    CssSelectorCompileErrorV1 error;

    const bool result = compile(
        ".one.two.three.four.five.six",
        &selector,
        &stats,
        &error);
    bool ok = expect(!result, "real ledger rejection must fail compile");
    ok &= expect(
        error.kind == CssSelectorCompileErrorKindV1::AllocationFailure,
        "real ledger rejection must surface allocation-failure");
    ok &= expect(
        ledger.snapshot(ResourceClass::ComputedStyle).rejected_reservations > 0U,
        "real ledger rejection must be visible in resource ledger");
    ok &= expect(ledger.accounting_clean(),
                 "ledger rejection accounting must remain clean");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_compilation_and_specificity();
    ok &= test_matching();
    ok &= test_quoted_attribute_values();
    ok &= test_universal_and_class_tokens();
    ok &= test_unsupported_and_invalid_syntax();
    ok &= test_match_bounds_and_corruption();
    ok &= test_atomic_failure_and_limits();
    ok &= test_real_ledger_rejection();
    if (!ok) {
        return 1;
    }
    std::cout << "CSS compound selector v1 tests passed\n";
    return 0;
}
