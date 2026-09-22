#include "css_parser_v1.hpp"

#include <cstddef>
#include <iostream>
#include <memory_resource>
#include <string_view>

namespace {

using namespace zevryon::style;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool parse(
    std::string_view source,
    CssStylesheetV1* sheet,
    CssParserV1Stats* stats,
    CssParserV1Error* error,
    CssParserV1Config config = {}) {
    return parse_css_stylesheet_v1(
        source,
        config,
        sheet,
        stats,
        error);
}

const CssDeclarationV1* find_declaration(
    const CssStylesheetV1& sheet,
    const CssStyleRuleV1& rule,
    std::string_view property) {
    const std::size_t first =
        static_cast<std::size_t>(rule.declaration_offset);
    const std::size_t count =
        static_cast<std::size_t>(rule.declaration_count);
    for (std::size_t index = 0U; index < count; ++index) {
        const CssDeclarationV1& declaration =
            sheet.declarations[first + index];
        if (sheet.resolve(declaration.property) == property) {
            return &declaration;
        }
    }
    return nullptr;
}

bool test_wpt_charset_is_not_a_rule() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    bool ok = expect(
        parse(
            "@charset \"utf-8\";"
            "@charset \"utf-8\";"
            "foo { color: blue; }"
            "@charset \"utf-8\";",
            &sheet,
            &stats,
            &error),
        "charset WPT slice must parse");
    ok &= expect(sheet.rules.size() == 1U,
                 "charset directives must not become style rules");
    ok &= expect(sheet.at_rules.empty(),
                 "charset directives must be dropped from at-rule sidecar");
    ok &= expect(stats.dropped_charset_rules == 3U,
                 "all three charset directives must be counted as dropped");
    ok &= expect(stats.at_rules == 0U,
                 "dropped charset directives must not count as retained at-rules");
    if (!sheet.rules.empty()) {
        ok &= expect(sheet.resolve(sheet.rules.front().selector) == "foo",
                     "qualified rule after charset directives must survive");
        const CssDeclarationV1* color =
            find_declaration(sheet, sheet.rules.front(), "color");
        ok &= expect(color != nullptr,
                     "surviving qualified rule must retain color declaration");
        if (color != nullptr) {
            ok &= expect(sheet.resolve(color->value) == "blue",
                         "surviving color value must be blue");
        }
    }
    return ok;
}

bool test_wpt_block_at_rule_inside_declaration_list() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    bool ok = expect(
        parse(
            "div { @at {} color: green; }",
            &sheet,
            &stats,
            &error),
        "declaration-list block at-rule WPT slice must parse");
    ok &= expect(sheet.rules.size() == 1U,
                 "block at-rule fixture must retain outer style rule");
    ok &= expect(sheet.at_rules.size() == 1U,
                 "block at-rule fixture must retain one at-rule record");
    if (!sheet.at_rules.empty()) {
        const CssAtRuleV1& rule = sheet.at_rules.front();
        ok &= expect(sheet.resolve(rule.name) == "at",
                     "at-rule name must canonicalize to at");
        ok &= expect(rule.context == CssAtRuleContextV1::DeclarationList,
                     "at-rule context must be declaration-list");
        ok &= expect(rule.owner_rule_index == 0U,
                     "declaration-list at-rule must retain owner rule zero");
        ok &= expect(rule.has_block,
                     "block at-rule must report block form");
        ok &= expect(sheet.resolve(rule.block).empty(),
                     "empty at-rule block must retain empty payload");
    }
    if (!sheet.rules.empty()) {
        const CssDeclarationV1* color =
            find_declaration(sheet, sheet.rules.front(), "color");
        ok &= expect(color != nullptr &&
                         sheet.resolve(color->value) == "green",
                     "declaration after block at-rule must survive");
    }
    return ok;
}

bool test_wpt_semicolon_at_rule_inside_declaration_list() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    bool ok = expect(
        parse(
            "div { @at at; color: green; }",
            &sheet,
            &stats,
            &error),
        "declaration-list semicolon at-rule WPT slice must parse");
    ok &= expect(sheet.at_rules.size() == 1U,
                 "semicolon at-rule fixture must retain one at-rule record");
    if (!sheet.at_rules.empty()) {
        const CssAtRuleV1& rule = sheet.at_rules.front();
        ok &= expect(rule.owner_rule_index == 0U,
                     "semicolon at-rule must retain owner rule zero");
        ok &= expect(!rule.has_block,
                     "semicolon at-rule must report no block");
        ok &= expect(sheet.resolve(rule.prelude) == "at",
                     "semicolon at-rule prelude must survive");
    }
    if (!sheet.rules.empty()) {
        const CssDeclarationV1* color =
            find_declaration(sheet, sheet.rules.front(), "color");
        ok &= expect(color != nullptr &&
                         sheet.resolve(color->value) == "green",
                     "declaration after semicolon at-rule must survive");
    }
    return ok;
}

bool test_top_level_at_rule_retention() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    bool ok = expect(
        parse(
            "@media screen { .a { color: red; } }"
            "div { color: green; }",
            &sheet,
            &stats,
            &error),
        "top-level generic at-rule must parse");
    ok &= expect(sheet.at_rules.size() == 1U,
                 "top-level generic at-rule must be retained");
    ok &= expect(sheet.rules.size() == 1U,
                 "qualified rule after top-level at-rule must survive");
    if (!sheet.at_rules.empty()) {
        const CssAtRuleV1& rule = sheet.at_rules.front();
        ok &= expect(rule.context == CssAtRuleContextV1::TopLevel,
                     "top-level at-rule context must be top-level");
        ok &= expect(rule.owner_rule_index == kCssAtRuleNoOwnerV1,
                     "top-level at-rule must not claim a style-rule owner");
        ok &= expect(rule.has_block,
                     "media at-rule must retain block form");
        ok &= expect(sheet.resolve(rule.name) == "media",
                     "media name must be retained");
        ok &= expect(sheet.resolve(rule.prelude) == "screen",
                     "media prelude must be trimmed");
        ok &= expect(
            sheet.resolve(rule.block).find(".a") != std::string_view::npos,
            "media raw block must retain nested content");
    }
    return ok;
}

bool test_declaration_list_owner_identity() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    bool ok = expect(
        parse(
            "a { @one; color: red; }"
            "b { @two {} color: blue; }",
            &sheet,
            &stats,
            &error),
        "multi-rule at-rule ownership fixture must parse");
    ok &= expect(sheet.rules.size() == 2U,
                 "ownership fixture must retain two style rules");
    ok &= expect(sheet.at_rules.size() == 2U,
                 "ownership fixture must retain two at-rules");
    if (sheet.at_rules.size() == 2U) {
        ok &= expect(
            sheet.at_rules[0].owner_rule_index == 0U &&
                sheet.at_rules[1].owner_rule_index == 1U,
            "declaration-list at-rules must retain distinct owner indices");
    }
    return ok;
}

bool test_bad_declaration_recovery() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    bool ok = expect(
        parse(
            "div {"
            "broken;"
            "color: green;"
            "1bad: value;"
            "co/*x*/lor: red;"
            "width: 2px;"
            "}",
            &sheet,
            &stats,
            &error),
        "recoverable bad declarations must not abort stylesheet");
    ok &= expect(error.kind == CssParserV1ErrorKind::None,
                 "successful recovery must clear parser error");
    ok &= expect(stats.recovered_invalid_declarations == 3U,
                 "three invalid declarations must be recovered");
    ok &= expect(sheet.rules.size() == 1U,
                 "recovery fixture must retain outer rule");
    if (!sheet.rules.empty()) {
        const CssStyleRuleV1& rule = sheet.rules.front();
        ok &= expect(rule.declaration_count == 2U,
                     "only two valid declarations must survive recovery");
        const CssDeclarationV1* color =
            find_declaration(sheet, rule, "color");
        const CssDeclarationV1* width =
            find_declaration(sheet, rule, "width");
        ok &= expect(color != nullptr &&
                         sheet.resolve(color->value) == "green",
                     "color declaration after bad remnant must survive");
        ok &= expect(width != nullptr &&
                         sheet.resolve(width->value) == "2px",
                     "width declaration after later bad remnant must survive");
    }
    return ok;
}

bool test_recovery_does_not_hide_structural_failure() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    bool ok = expect(
        parse(
            "div { color: green; }",
            &sheet,
            &stats,
            &error),
        "atomic recovery baseline must parse");
    const auto baseline_rules = sheet.rules.size();
    const auto baseline_declarations = sheet.declarations.size();

    ok &= expect(
        !parse(
            "div { @at (foo",
            &sheet,
            &stats,
            &error),
        "unterminated at-rule prelude must remain fatal");
    ok &= expect(
        error.kind == CssParserV1ErrorKind::UnbalancedBlock,
        "unterminated at-rule prelude must report unbalanced-block");
    ok &= expect(
        sheet.rules.size() == baseline_rules &&
            sheet.declarations.size() == baseline_declarations,
        "fatal at-rule parse must preserve previous output atomically");
    return ok;
}

bool test_at_rule_name_boundaries() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;
    bool ok = true;

    ok &= expect(
        !parse("@-;", &sheet, &stats, &error),
        "single hyphen must not form an at-rule identifier");
    ok &= expect(
        error.kind == CssParserV1ErrorKind::InvalidAtRule,
        "single-hyphen at-rule must report invalid-at-rule");

    ok &= expect(
        !parse("@-1;", &sheet, &stats, &error),
        "hyphen-digit must not form an at-rule identifier");
    ok &= expect(
        error.kind == CssParserV1ErrorKind::InvalidAtRule,
        "hyphen-digit at-rule must report invalid-at-rule");

    ok &= expect(
        !parse("@\\6d edia;", &sheet, &stats, &error),
        "escaped at-rule name must fail closed in this foundation");
    ok &= expect(
        error.kind == CssParserV1ErrorKind::UnsupportedSyntax,
        "escaped at-rule name must report unsupported-syntax");
    return ok;
}

bool test_at_rule_block_nesting_limit() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;
    CssParserV1Config config;
    config.maximum_nesting_depth = 1U;

    bool ok = expect(
        !parse(
            "@media { (nested) }",
            &sheet,
            &stats,
            &error,
            config),
        "nested component value inside at-rule block must respect nesting limit");
    ok &= expect(
        error.kind == CssParserV1ErrorKind::NestingLimitExceeded,
        "at-rule block nesting limit must report exact error");
    return ok;
}

bool test_at_rule_limit() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 sheet(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;
    CssParserV1Config config;
    config.maximum_at_rules = 1U;

    bool ok = expect(
        !parse(
            "@one; @two;",
            &sheet,
            &stats,
            &error,
            config),
        "at-rule count limit must fail closed");
    ok &= expect(
        error.kind == CssParserV1ErrorKind::AtRuleLimitExceeded,
        "at-rule count limit must report exact error");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_wpt_charset_is_not_a_rule();
    ok &= test_wpt_block_at_rule_inside_declaration_list();
    ok &= test_wpt_semicolon_at_rule_inside_declaration_list();
    ok &= test_top_level_at_rule_retention();
    ok &= test_declaration_list_owner_identity();
    ok &= test_bad_declaration_recovery();
    ok &= test_recovery_does_not_hide_structural_failure();
    ok &= test_at_rule_name_boundaries();
    ok &= test_at_rule_block_nesting_limit();
    ok &= test_at_rule_limit();
    if (!ok) {
        return 1;
    }
    std::cout << "CSS at-rule and recovery v1 tests passed\n";
    return 0;
}
