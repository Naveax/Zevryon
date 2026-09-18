#include "css_parser_v1.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
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

bool parse(
    std::string_view input,
    CssStylesheetV1* sheet,
    CssParserV1Stats* stats,
    CssParserV1Error* error,
    CssParserV1Config config = {}) {
    return parse_css_stylesheet_v1(input, config, sheet, stats, error);
}

bool test_basic_rules_and_canonical_properties() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    const std::string_view source =
        "  .card, #hero { COLOR: red; margin: 0 1px; }\n"
        "p[data-x=\"a;b\"] { font-family: \"A; B\"; opacity: .5 !IMPORTANT; }";
    bool ok = expect(parse(source, &sheet, &stats, &error), "basic stylesheet must parse");
    ok &= expect(error.kind == CssParserV1ErrorKind::None, "basic parse error must be clear");
    ok &= expect(sheet.rules.size() == 2U, "two style rules must be emitted");
    ok &= expect(sheet.declarations.size() == 4U, "four declarations must be emitted");
    ok &= expect(sheet.resolve(sheet.rules[0].selector) == ".card, #hero", "first selector must be preserved");
    ok &= expect(sheet.resolve(sheet.declarations[0].property) == "color", "normal properties must canonicalize to lowercase");
    ok &= expect(sheet.resolve(sheet.declarations[0].value) == "red", "first value must be preserved");
    ok &= expect(sheet.resolve(sheet.declarations[3].property) == "opacity", "opacity property must resolve");
    ok &= expect(sheet.resolve(sheet.declarations[3].value) == ".5", "important suffix must be removed from value");
    ok &= expect(sheet.declarations[3].important, "important marker must be detected case-insensitively");
    ok &= expect(stats.rules == 2U && stats.declarations == 4U, "basic stats must be exact");
    ok &= expect(stats.important_declarations == 1U, "important stats must be exact");
    ok &= expect(stats.output_text_bytes == sheet.text.size(), "text byte stats must match retained text");
    ok &= expect(ledger.accounting_clean(), "basic parser accounting must remain clean");
    ok &= expect(ledger.within_hard_limits(), "basic parser must remain inside ledger limit");
    return ok;
}

bool test_comments_functions_custom_properties_and_balancing() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    const std::string_view source =
        "/* lead */ .x:not([data-v=\"x;y\"]) {"
        " background: linear-gradient(rgb(1, 2, 3), var(--accent));"
        " --ThemeToken: {fg: white; nested: [a(b)]};"
        " content: \"/* not a comment */ ; }\";"
        " } /* tail */";
    bool ok = expect(parse(source, &sheet, &stats, &error), "nested syntax fixture must parse");
    ok &= expect(sheet.rules.size() == 1U, "nested fixture must emit one rule");
    ok &= expect(sheet.declarations.size() == 3U, "nested fixture must emit three declarations");
    ok &= expect(sheet.resolve(sheet.declarations[1].property) == "--ThemeToken", "custom property case must be preserved");
    ok &= expect(sheet.resolve(sheet.declarations[1].value) == "{fg: white; nested: [a(b)]}", "balanced custom-property block must be retained");
    ok &= expect(sheet.resolve(sheet.declarations[2].value) == "\"/* not a comment */ ; }\"", "string delimiters must not terminate declarations");
    ok &= expect(stats.comments == 2U, "only real comments must be counted");
    ok &= expect(stats.maximum_nesting_depth >= 2U, "nested functions must update depth stats");
    ok &= expect(ledger.accounting_clean(), "nested parser accounting must remain clean");
    return ok;
}

bool test_failure_is_atomic() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    bool ok = expect(parse(".ok { color: green; }", &sheet, &stats, &error), "seed stylesheet must parse");
    const std::string previous_text(sheet.text.data(), sheet.text.size());
    const std::size_t previous_rules = sheet.rules.size();
    const std::size_t previous_declarations = sheet.declarations.size();

    ok &= expect(!parse(".broken { color red; }", &sheet, &stats, &error), "missing colon must fail");
    ok &= expect(error.kind == CssParserV1ErrorKind::InvalidDeclaration, "missing colon error kind must be exact");
    ok &= expect(std::string_view(sheet.text.data(), sheet.text.size()) == previous_text, "failed parse must preserve previous text");
    ok &= expect(sheet.rules.size() == previous_rules, "failed parse must preserve previous rules");
    ok &= expect(sheet.declarations.size() == previous_declarations, "failed parse must preserve previous declarations");
    ok &= expect(ledger.accounting_clean(), "atomic failure accounting must remain clean");
    return ok;
}

bool test_strict_syntax_boundaries() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;
    bool ok = true;

    ok &= expect(!parse("@media screen { .x { color: red; } }", &sheet, &stats, &error), "at-rules must fail outside foundation scope");
    ok &= expect(error.kind == CssParserV1ErrorKind::UnsupportedSyntax, "at-rule failure kind must be exact");
    ok &= expect(!parse(".x { content: \"unterminated; }", &sheet, &stats, &error), "unterminated string must fail");
    ok &= expect(error.kind == CssParserV1ErrorKind::UnterminatedString, "unterminated string kind must be exact");
    ok &= expect(!parse("/* unterminated", &sheet, &stats, &error), "unterminated comment must fail");
    ok &= expect(error.kind == CssParserV1ErrorKind::UnterminatedComment, "unterminated comment kind must be exact");
    ok &= expect(!parse(".x[a=(b] { color: red; }", &sheet, &stats, &error), "mismatched selector delimiters must fail");
    ok &= expect(error.kind == CssParserV1ErrorKind::InvalidSelector, "selector mismatch kind must be exact");
    ok &= expect(!parse(".x { color: rgb(1, 2]; }", &sheet, &stats, &error), "mismatched value delimiters must fail");
    ok &= expect(error.kind == CssParserV1ErrorKind::InvalidDeclaration, "value mismatch kind must be exact");
    return ok;
}

bool test_explicit_limits() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;
    bool ok = true;

    CssParserV1Config config;
    config.maximum_input_bytes = 8U;
    ok &= expect(!parse(".x { color: red; }", &sheet, &stats, &error, config), "input byte limit must fail closed");
    ok &= expect(error.kind == CssParserV1ErrorKind::InputTooLarge, "input byte limit kind must be exact");

    config = CssParserV1Config{};
    config.maximum_rules = 1U;
    ok &= expect(!parse("a{x:1;} b{y:2;}", &sheet, &stats, &error, config), "rule count limit must fail closed");
    ok &= expect(error.kind == CssParserV1ErrorKind::RuleLimitExceeded, "rule limit kind must be exact");

    config = CssParserV1Config{};
    config.maximum_declarations = 1U;
    ok &= expect(!parse("a{x:1;y:2;}", &sheet, &stats, &error, config), "declaration count limit must fail closed");
    ok &= expect(error.kind == CssParserV1ErrorKind::DeclarationLimitExceeded, "declaration limit kind must be exact");

    config = CssParserV1Config{};
    config.maximum_nesting_depth = 1U;
    ok &= expect(!parse("a{x:fn(inner(1));}", &sheet, &stats, &error, config), "nesting limit must fail closed");
    ok &= expect(error.kind == CssParserV1ErrorKind::NestingLimitExceeded, "nesting limit kind must be exact");

    config = CssParserV1Config{};
    config.maximum_output_text_bytes = 4U;
    ok &= expect(!parse("abcd{x:1;}", &sheet, &stats, &error, config), "logical output text limit must fail closed");
    ok &= expect(error.kind == CssParserV1ErrorKind::OutputBudgetExceeded, "output budget kind must be exact");
    return ok;
}

bool test_computed_style_ledger_rejection() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 64U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    const std::string source =
        ".this-selector-is-intentionally-long { very-long-property-name: " +
        std::string(256U, 'x') + "; }";
    bool ok = expect(!parse(source, &sheet, &stats, &error), "ledger hard limit must reject parser allocation");
    ok &= expect(error.kind == CssParserV1ErrorKind::AllocationFailure, "ledger rejection must classify as allocation failure");
    const auto snapshot = ledger.snapshot(ResourceClass::ComputedStyle);
    ok &= expect(snapshot.rejected_reservations > 0U, "ledger must record rejected reservation");
    ok &= expect(snapshot.current_bytes == 0U, "failed candidate allocations must unwind completely");
    ok &= expect(ledger.accounting_clean(), "ledger rejection must not corrupt accounting");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_basic_rules_and_canonical_properties();
    ok &= test_comments_functions_custom_properties_and_balancing();
    ok &= test_failure_is_atomic();
    ok &= test_strict_syntax_boundaries();
    ok &= test_explicit_limits();
    ok &= test_computed_style_ledger_rejection();
    if (!ok) {
        return 1;
    }
    std::cout << "CSS parser v1 foundation tests passed\n";
    return 0;
}
