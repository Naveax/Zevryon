#include "css_inline_cascade_merge_v1.hpp"
#include "css_style_dag_v1.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace zevryon::style;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr
            << "FAILED: Z3 inline cascade merge: "
            << message << '\n';
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

bool cascade_div(
    const CssStylesheetV1& sheet,
    CssCascadeResultV1* result) {
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    return cascade_css_author_rules_v1(
        sheet,
        CssSelectorNodeV1{"div", {}},
        CssCascadeConfigV1{},
        result,
        &stats,
        &error);
}

const CssCascadeWinnerV1* find_winner(
    const CssInlineMergedCascadeV1& merged,
    std::string_view property) {
    for (const CssCascadeWinnerV1& winner :
         merged.cascade.winners) {
        if (merged.stylesheet.resolve(winner.property) ==
            property) {
            return &winner;
        }
    }
    return nullptr;
}

bool test_inline_origin_precedence_and_dag_compatibility() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 author_sheet(&memory);
    CssCascadeResultV1 author_cascade(&memory);
    CssStylesheetV1 inline_sheet(&memory);

    if (!require(
            parse_sheet(
                "div{"
                "color:red!important;"
                "margin:1px;"
                "padding:3px;"
                "}",
                &author_sheet) &&
                cascade_div(
                    author_sheet,
                    &author_cascade),
            "author stylesheet and cascade must succeed")) {
        return false;
    }
    if (!require(
            parse_sheet(
                "*{"
                "color:blue;"
                "margin:2px!important;"
                "padding:4px;"
                "border:5px;"
                "}",
                &inline_sheet),
            "synthetic inline declaration sheet must parse")) {
        return false;
    }

    CssInlineMergedCascadeV1 merged(&memory);
    CssInlineCascadeMergeStatsV1 stats;
    CssInlineCascadeMergeErrorV1 error;
    if (!require(
            merge_css_author_and_inline_cascade_v1(
                author_sheet,
                author_cascade,
                inline_sheet,
                inline_sheet.declarations,
                CssInlineCascadeMergeConfigV1{},
                &merged,
                &stats,
                &error),
            "author and inline declarations must merge")) {
        return false;
    }

    const CssCascadeWinnerV1* color =
        find_winner(merged, "color");
    const CssCascadeWinnerV1* margin =
        find_winner(merged, "margin");
    const CssCascadeWinnerV1* padding =
        find_winner(merged, "padding");
    const CssCascadeWinnerV1* border =
        find_winner(merged, "border");

    if (!require(
            color != nullptr &&
                merged.stylesheet.resolve(color->value) == "red" &&
                color->important,
            "author important must beat inline normal")) {
        return false;
    }
    if (!require(
            margin != nullptr &&
                merged.stylesheet.resolve(margin->value) == "2px" &&
                margin->important,
            "inline important must beat author normal")) {
        return false;
    }
    if (!require(
            padding != nullptr &&
                merged.stylesheet.resolve(padding->value) == "4px" &&
                !padding->important,
            "inline normal must beat author normal at equal importance")) {
        return false;
    }
    if (!require(
            border != nullptr &&
                merged.stylesheet.resolve(border->value) == "5px",
            "inline-only property must be retained")) {
        return false;
    }
    if (!require(
            stats.author_properties == 3U &&
                stats.inline_declarations == 4U &&
                stats.inline_overrides_author == 2U &&
                stats.author_important_preserved == 1U &&
                stats.output_properties == 4U,
            "inline merge authority counters must be exact")) {
        return false;
    }

    CssComputedStyleDagV1 dag(&memory);
    std::uint32_t terminal = kCssStyleDagNoNodeV1;
    CssStyleDagStatsV1 dag_stats;
    CssStyleDagErrorV1 dag_error;
    if (!require(
            intern_css_cascade_style_v1(
                merged.stylesheet,
                merged.cascade,
                CssStyleDagConfigV1{},
                &dag,
                &terminal,
                &dag_stats,
                &dag_error),
            "merged final winner set must feed production style DAG")) {
        return false;
    }

    return require(
        terminal != kCssStyleDagNoNodeV1 &&
            dag.nodes[terminal].depth == 4U,
        "merged final style must intern as four canonical properties");
}

bool test_inline_duplicate_and_custom_property_semantics() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 author_sheet(&memory);
    CssCascadeResultV1 author_cascade(&memory);
    CssStylesheetV1 inline_sheet(&memory);

    if (!require(
            parse_sheet(
                "*{"
                "color:red;"
                "color:blue!important;"
                "color:green;"
                "--X:a;"
                "--x:b;"
                "}",
                &inline_sheet),
            "duplicate inline declaration sheet must parse")) {
        return false;
    }

    CssInlineMergedCascadeV1 merged(&memory);
    CssInlineCascadeMergeStatsV1 stats;
    CssInlineCascadeMergeErrorV1 error;
    if (!require(
            merge_css_author_and_inline_cascade_v1(
                author_sheet,
                author_cascade,
                inline_sheet,
                inline_sheet.declarations,
                CssInlineCascadeMergeConfigV1{},
                &merged,
                &stats,
                &error),
            "duplicate inline declarations must merge")) {
        return false;
    }

    const CssCascadeWinnerV1* color =
        find_winner(merged, "color");
    if (!require(
            color != nullptr &&
                merged.stylesheet.resolve(color->value) == "blue" &&
                color->important,
            "inline important must survive later normal duplicate")) {
        return false;
    }
    if (!require(
            find_winner(merged, "--X") != nullptr &&
                find_winner(merged, "--x") != nullptr,
            "custom property identity must remain case-sensitive")) {
        return false;
    }

    return require(
        stats.inline_duplicate_declarations == 2U &&
            stats.inline_duplicate_replacements == 1U &&
            stats.output_properties == 3U,
        "duplicate inline authority counters must be exact");
}

bool test_failure_preserves_prior_merged_output() {
    std::pmr::monotonic_buffer_resource memory;
    CssStylesheetV1 author_sheet(&memory);
    CssCascadeResultV1 author_cascade(&memory);
    CssStylesheetV1 inline_sheet(&memory);

    if (!require(
            parse_sheet("div{a:1;}", &author_sheet) &&
                cascade_div(
                    author_sheet,
                    &author_cascade) &&
                parse_sheet("*{b:2;}", &inline_sheet),
            "failure fixture must parse and cascade")) {
        return false;
    }

    CssInlineMergedCascadeV1 merged(&memory);
    CssInlineCascadeMergeStatsV1 stats;
    CssInlineCascadeMergeErrorV1 error;
    if (!require(
            merge_css_author_and_inline_cascade_v1(
                author_sheet,
                author_cascade,
                inline_sheet,
                inline_sheet.declarations,
                CssInlineCascadeMergeConfigV1{},
                &merged,
                &stats,
                &error),
            "failure baseline merge must succeed")) {
        return false;
    }

    const std::string before_text(
        merged.stylesheet.text);
    const std::size_t before_winners =
        merged.cascade.winners.size();

    CssInlineCascadeMergeConfigV1 tiny_text;
    tiny_text.maximum_output_text_bytes = 1U;
    if (!require(
            !merge_css_author_and_inline_cascade_v1(
                author_sheet,
                author_cascade,
                inline_sheet,
                inline_sheet.declarations,
                tiny_text,
                &merged,
                &stats,
                &error) &&
                error.kind ==
                    CssInlineCascadeMergeErrorKindV1::TextBudgetExceeded,
            "tiny output text budget must fail closed")) {
        return false;
    }
    if (!require(
            std::string_view(
                merged.stylesheet.text.data(),
                merged.stylesheet.text.size()) ==
                    std::string_view(before_text) &&
                merged.cascade.winners.size() ==
                    before_winners,
            "failed inline merge must preserve prior published output")) {
        return false;
    }

    CssDeclarationV1 invalid =
        inline_sheet.declarations.front();
    invalid.property.offset =
        std::numeric_limits<std::uint32_t>::max();
    const std::array<CssDeclarationV1, 1>
        invalid_list{{invalid}};

    return require(
        !merge_css_author_and_inline_cascade_v1(
            author_sheet,
            author_cascade,
            inline_sheet,
            invalid_list,
            CssInlineCascadeMergeConfigV1{},
            &merged,
            &stats,
            &error) &&
            error.kind ==
                CssInlineCascadeMergeErrorKindV1::InvalidInlineDeclaration,
        "invalid inline declaration slice must fail closed");
}

} // namespace

int main() {
    if (!test_inline_origin_precedence_and_dag_compatibility() ||
        !test_inline_duplicate_and_custom_property_semantics() ||
        !test_failure_preserves_prior_merged_output()) {
        return 1;
    }

    std::cout
        << "Z3 inline cascade merge v1 tests passed\n";
    return 0;
}
