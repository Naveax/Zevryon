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
    ok &= expect(stats.input_bytes == source.size(), "raw input byte stats must be exact");
    ok &= expect(stats.preprocessed_input_bytes == source.size(), "clean UTF-8 preprocessing must preserve byte count");
    ok &= expect(stats.null_replacements == 0U, "clean input must not replace NUL");
    ok &= expect(stats.newline_normalizations == 0U, "clean input must not normalize newlines");
    ok &= expect(stats.invalid_utf8_replacements == 0U, "clean input must not replace UTF-8");
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

bool test_css_input_preprocessing() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;
    const std::string replacement("\xef\xbf\xbd", 3U);
    bool ok = true;

    auto check_null_case = [&](std::string selector, const std::string& expected, std::uint64_t expected_replacements) {
        const std::string source = selector + " { x: 1; }";
        bool local = expect(parse(source, &sheet, &stats, &error), "WPT NUL preprocessing case must parse");
        local &= expect(sheet.rules.size() == 1U, "WPT NUL case must emit one rule");
        if (!sheet.rules.empty()) {
            local &= expect(sheet.resolve(sheet.rules[0].selector) == expected, "WPT NUL selector preprocessing must match");
        }
        local &= expect(stats.input_bytes == source.size(), "WPT NUL raw byte count must match");
        local &= expect(stats.null_replacements == expected_replacements, "WPT NUL replacement count must match");
        local &= expect(stats.invalid_utf8_replacements == 0U, "WPT NUL case must not classify as invalid UTF-8");
        local &= expect(
            stats.preprocessed_input_bytes ==
                stats.input_bytes + (2U * expected_replacements),
            "WPT NUL expansion byte count must match");
        return local;
    };

    std::string selector = "foo";
    selector.push_back('\0');
    ok &= check_null_case(selector, std::string("foo") + replacement, 1U);

    selector = "f";
    selector.push_back('\0');
    selector += "oo";
    ok &= check_null_case(selector, std::string("f") + replacement + "oo", 1U);

    selector.clear();
    selector.push_back('\0');
    selector += "foo";
    ok &= check_null_case(selector, replacement + "foo", 1U);

    selector.assign(1U, '\0');
    ok &= check_null_case(selector, replacement, 1U);

    selector.assign(3U, '\0');
    ok &= check_null_case(selector, replacement + replacement + replacement, 3U);

    const std::string newline_source = ".a\r\n.b\r.c\f.d { x: 1; }";
    ok &= expect(parse(newline_source, &sheet, &stats, &error), "CSS newline preprocessing case must parse");
    ok &= expect(sheet.resolve(sheet.rules[0].selector) == ".a\n.b\n.c\n.d", "CRLF CR and FF must normalize to LF");
    ok &= expect(stats.newline_normalizations == 3U, "newline normalization count must be exact");
    ok &= expect(stats.preprocessed_input_bytes + 1U == stats.input_bytes, "CRLF normalization must remove exactly one byte");

    std::string invalid_source = ".x";
    invalid_source.push_back(static_cast<char>(0xedU));
    invalid_source.push_back(static_cast<char>(0xa0U));
    invalid_source.push_back(static_cast<char>(0x80U));
    invalid_source += " { x: 1; }";
    ok &= expect(parse(invalid_source, &sheet, &stats, &error), "surrogate-encoded UTF-8 must be replaced and parsed");
    ok &= expect(sheet.resolve(sheet.rules[0].selector) == std::string(".x") + replacement, "surrogate sequence must become one replacement character");
    ok &= expect(stats.invalid_utf8_replacements == 1U, "surrogate sequence replacement count must be exact");
    ok &= expect(stats.null_replacements == 0U, "surrogate sequence must not affect NUL count");

    ok &= expect(ledger.accounting_clean(), "preprocessing accounting must remain clean");
    ok &= expect(ledger.within_hard_limits(), "preprocessing must remain inside ledger limit");
    return ok;
}

bool test_wpt_declaration_list_authority() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;
    bool ok = true;

    const std::string_view whitespace_source =
        "#foo {"
        " --foo-1:bar;"
        " --foo-2: bar;"
        " --foo-3:bar ;"
        " --foo-4: bar ;"
        " --foo-5: bar !important;"
        " --foo-6: bar !important ;"
        " --foo-7:bar!important;"
        " --foo-8:bar!important ;"
        " --foo-9:bar"
        "}";

    ok &= expect(
        parse(whitespace_source, &sheet, &stats, &error),
        "WPT declaration whitespace authority must parse");
    ok &= expect(
        sheet.rules.size() == 1U,
        "WPT declaration whitespace authority must emit one rule");
    ok &= expect(
        sheet.declarations.size() == 9U,
        "WPT declaration whitespace authority must emit nine declarations");
    for (std::size_t index = 0U; index < sheet.declarations.size(); ++index) {
        const CssDeclarationV1& declaration = sheet.declarations[index];
        ok &= expect(
            sheet.resolve(declaration.value) == "bar",
            "WPT declaration values must trim surrounding whitespace and important");
        const bool expected_important = index >= 4U && index <= 7U;
        ok &= expect(
            declaration.important == expected_important,
            "WPT important markers must match the frozen nine-case matrix");
    }
    ok &= expect(
        stats.declarations == 9U,
        "WPT declaration whitespace stats must report nine declarations");
    ok &= expect(
        stats.important_declarations == 4U,
        "WPT declaration whitespace stats must report four important declarations");

    const std::string_view missing_semicolon_source =
        ".c {"
        " /* This { needs to be there to send Chromium into a different path. */"
        " color: red;"
        " color: green"
        "}";
    ok &= expect(
        parse(missing_semicolon_source, &sheet, &stats, &error),
        "WPT missing-semicolon authority must parse");
    ok &= expect(
        sheet.rules.size() == 1U && sheet.declarations.size() == 2U,
        "WPT missing-semicolon authority must retain both declarations");
    if (sheet.declarations.size() == 2U) {
        ok &= expect(
            sheet.resolve(sheet.declarations[0].property) == "color" &&
                sheet.resolve(sheet.declarations[0].value) == "red",
            "first missing-semicolon fixture declaration must remain red");
        ok &= expect(
            sheet.resolve(sheet.declarations[1].property) == "color" &&
                sheet.resolve(sheet.declarations[1].value) == "green",
            "final declaration without semicolon must remain green");
    }
    ok &= expect(
        stats.comments == 1U,
        "brace inside WPT fixture comment must not alter block parsing");
    ok &= expect(
        ledger.accounting_clean(),
        "WPT declaration-list authority accounting must remain clean");
    ok &= expect(
        ledger.within_hard_limits(),
        "WPT declaration-list authority must remain within ledger limit");
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

    ok &= expect(!parse(".broken { content: \"unterminated; }", &sheet, &stats, &error), "structural string failure must remain fatal");
    ok &= expect(error.kind == CssParserV1ErrorKind::UnterminatedString, "fatal string error kind must be exact");
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

    ok &= expect(parse("@media screen { .x { color: red; } }", &sheet, &stats, &error), "generic top-level at-rule must be retained");
    ok &= expect(sheet.at_rules.size() == 1U && sheet.resolve(sheet.at_rules.front().name) == "media", "retained top-level at-rule must be media");
    ok &= expect(!parse(".x { content: \"unterminated; }", &sheet, &stats, &error), "unterminated string must fail");
    ok &= expect(error.kind == CssParserV1ErrorKind::UnterminatedString, "unterminated string kind must be exact");
    ok &= expect(!parse("/* unterminated", &sheet, &stats, &error), "unterminated comment must fail");
    ok &= expect(error.kind == CssParserV1ErrorKind::UnterminatedComment, "unterminated comment kind must be exact");
    ok &= expect(!parse(".x[a=(b] { color: red; }", &sheet, &stats, &error), "mismatched selector delimiters must fail");
    ok &= expect(error.kind == CssParserV1ErrorKind::InvalidSelector, "selector mismatch kind must be exact");
    ok &= expect(parse(".x { color: rgb(1, 2]; width: 2px; }", &sheet, &stats, &error), "mismatched declaration value must recover");
    ok &= expect(error.kind == CssParserV1ErrorKind::None, "recovered value mismatch must clear error");
    ok &= expect(stats.recovered_invalid_declarations == 1U, "value mismatch recovery count must be exact");
    ok &= expect(sheet.rules.size() == 1U && sheet.rules.front().declaration_count == 1U, "only declaration after recovered mismatch must survive");
    ok &= expect(sheet.resolve(sheet.declarations.front().property) == "width", "width declaration must survive recovered mismatch");
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
    ok &= test_css_input_preprocessing();
    ok &= test_wpt_declaration_list_authority();
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
