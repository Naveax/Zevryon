#include "css_parser_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <string>
#include <string_view>

namespace {

using namespace zevryon::style;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr
            << "FAILED: CSS declaration-list v1: "
            << message << '\n';
        return false;
    }
    return true;
}

bool parse_list(
    std::string_view input,
    CssStylesheetV1* output,
    CssParserV1Stats* stats,
    CssParserV1Error* error) {
    return parse_css_declaration_list_v1(
        input,
        CssParserV1Config{},
        output,
        stats,
        error);
}

bool test_eof_terminated_inline_like_declarations() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 output(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    if (!require(
            parse_list(
                " color:red; MARGIN: 2px!important; --X: y ",
                &output,
                &stats,
                &error),
            "EOF-terminated declaration list must parse")) {
        return false;
    }

    if (!require(
            output.rules.empty() &&
                output.at_rules.empty() &&
                output.declarations.size() == 3U,
            "standalone list must publish declarations without synthetic qualified rule")) {
        return false;
    }

    const CssDeclarationV1& color = output.declarations[0];
    const CssDeclarationV1& margin = output.declarations[1];
    const CssDeclarationV1& custom = output.declarations[2];
    if (!require(
            output.resolve(color.property) == "color" &&
                output.resolve(color.value) == "red" &&
                !color.important,
            "normal declaration must round trip")) {
        return false;
    }
    if (!require(
            output.resolve(margin.property) == "margin" &&
                output.resolve(margin.value) == "2px" &&
                margin.important,
            "normal property name must canonicalize and important must survive")) {
        return false;
    }
    if (!require(
            output.resolve(custom.property) == "--X" &&
                output.resolve(custom.value) == "y",
            "custom property case must be preserved")) {
        return false;
    }

    return require(
        stats.rules == 0U &&
            stats.declarations == 3U &&
            stats.important_declarations == 1U,
        "standalone declaration-list stats must not invent a qualified rule");
}

bool test_recovery_and_declaration_list_at_rule() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 output(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    if (!require(
            parse_list(
                "broken;@foo x;color:green;1bad:v;width:2px",
                &output,
                &stats,
                &error),
            "bounded invalid declarations must recover in standalone list")) {
        return false;
    }

    if (!require(
            output.rules.empty() &&
                output.declarations.size() == 2U &&
                output.at_rules.size() == 1U,
            "recovery list must retain only valid declarations plus bounded at-rule sidecar")) {
        return false;
    }
    if (!require(
            output.resolve(output.declarations[0].property) == "color" &&
                output.resolve(output.declarations[0].value) == "green" &&
                output.resolve(output.declarations[1].property) == "width" &&
                output.resolve(output.declarations[1].value) == "2px",
            "valid declarations after recovery must survive")) {
        return false;
    }

    const CssAtRuleV1& at = output.at_rules[0];
    if (!require(
            at.context == CssAtRuleContextV1::DeclarationList &&
                at.owner_rule_index == kCssAtRuleNoOwnerV1 &&
                !at.has_block &&
                output.resolve(at.name) == "foo" &&
                output.resolve(at.prelude) == "x",
            "standalone declaration-list at-rule must have declaration context and no synthetic owner")) {
        return false;
    }

    return require(
        stats.recovered_invalid_declarations == 2U &&
            stats.at_rules == 1U,
        "recovery and at-rule counters must be exact");
}

bool test_mismatched_value_recovers_before_final_declaration() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 output(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    if (!require(
            parse_list(
                "a:rgb(1,2];width:2px",
                &output,
                &stats,
                &error),
            "mismatched declaration value must recover to following declaration")) {
        return false;
    }

    return require(
        output.declarations.size() == 1U &&
            output.resolve(output.declarations[0].property) == "width" &&
            output.resolve(output.declarations[0].value) == "2px" &&
            stats.recovered_invalid_declarations == 1U,
        "recovery must preserve the valid final EOF-terminated declaration");
}

bool test_user_closing_brace_fails_and_preserves_output() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 output(&memory);
    CssParserV1Stats stats;
    CssParserV1Error error;

    if (!require(
            parse_list("a:1", &output, &stats, &error),
            "atomic baseline declaration list must parse")) {
        return false;
    }
    const std::string text_before(output.text);
    const std::size_t declarations_before =
        output.declarations.size();

    if (!require(
            !parse_list(
                "b:2}c:3",
                &output,
                &stats,
                &error),
            "user-provided raw closing brace must fail standalone list")) {
        return false;
    }
    if (!require(
            error.kind ==
                CssParserV1ErrorKind::UnbalancedBlock,
            "unexpected raw closing brace must report unbalanced-block")) {
        return false;
    }

    return require(
        std::string_view(output.text.data(), output.text.size()) ==
                std::string_view(text_before) &&
            output.declarations.size() ==
                declarations_before &&
            output.resolve(output.declarations[0].property) == "a" &&
            output.resolve(output.declarations[0].value) == "1",
        "failed standalone declaration-list parse must preserve prior output atomically");
}

} // namespace

int main() {
    if (!test_eof_terminated_inline_like_declarations() ||
        !test_recovery_and_declaration_list_at_rule() ||
        !test_mismatched_value_recovers_before_final_declaration() ||
        !test_user_closing_brace_fails_and_preserves_output()) {
        return 1;
    }

    std::cout
        << "CSS declaration-list v1 tests passed\n";
    return 0;
}
