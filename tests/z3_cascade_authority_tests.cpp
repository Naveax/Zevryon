#include "css_cascade_v1.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <string>
#include <string_view>

namespace {

using zevryon::core::LedgerMemoryResource;
using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;
using namespace zevryon::style;

struct SelectorProfile {
    std::string_view source;
    CssSpecificityV1 specificity;
};

constexpr std::array<SelectorProfile, 4> kProfiles{{
    {"*", {0U, 0U, 0U}},
    {"div", {0U, 0U, 1U}},
    {".card", {0U, 1U, 0U}},
    {"#hero", {1U, 0U, 0U}},
}};

constexpr std::size_t kProfileCount = kProfiles.size();
constexpr std::size_t kImportanceStates = 2U;
constexpr std::size_t kDecisionDenominator =
    kProfileCount * kProfileCount *
    kImportanceStates * kImportanceStates;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: Z3 cascade authority: "
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

std::string rule(
    std::string_view selector,
    std::string_view value,
    bool important) {
    std::string output(selector);
    output += "{color:";
    output += value;
    if (important) {
        output += "!important";
    }
    output += ";}";
    return output;
}

bool second_wins(
    std::size_t first_profile,
    bool first_important,
    std::size_t second_profile,
    bool second_important) noexcept {
    if (first_important != second_important) {
        return second_important;
    }
    if (first_profile != second_profile) {
        return second_profile > first_profile;
    }
    return true;
}

bool run_precedence_matrix() {
    ResourceLedger ledger;
    ledger.set_hard_limit(
        ResourceClass::ComputedStyle,
        4U * 1024U * 1024U);
    LedgerMemoryResource memory(
        ledger,
        ResourceClass::ComputedStyle);
    CssCascadeResultV1 result(&memory);

    const std::array<CssSelectorAttributeV1, 2> attributes{{
        {"id", "hero"},
        {"class", "card"},
    }};
    const CssSelectorNodeV1 node{"div", attributes};

    std::size_t decisions = 0U;
    std::size_t first_wins = 0U;
    std::size_t second_wins_count = 0U;
    std::size_t importance_replacements = 0U;
    std::size_t specificity_replacements = 0U;
    std::size_t source_order_replacements = 0U;

    for (std::size_t first_profile = 0U;
         first_profile < kProfileCount;
         ++first_profile) {
        for (std::size_t second_profile = 0U;
             second_profile < kProfileCount;
             ++second_profile) {
            for (std::size_t first_importance = 0U;
                 first_importance < kImportanceStates;
                 ++first_importance) {
                for (std::size_t second_importance = 0U;
                     second_importance < kImportanceStates;
                     ++second_importance) {
                    const bool first_important =
                        first_importance != 0U;
                    const bool second_important =
                        second_importance != 0U;

                    const std::string css =
                        rule(
                            kProfiles[first_profile].source,
                            "red",
                            first_important) +
                        rule(
                            kProfiles[second_profile].source,
                            "green",
                            second_important);

                    std::pmr::monotonic_buffer_resource sheet_memory;
                    CssStylesheetV1 sheet(&sheet_memory);
                    if (!require(
                            parse_sheet(css, &sheet),
                            "every precedence matrix stylesheet must parse")) {
                        return false;
                    }
                    if (!require(
                            sheet.rules.size() == 2U &&
                                sheet.declarations.size() == 2U,
                            "every precedence matrix stylesheet must contain two declarations")) {
                        return false;
                    }

                    CssCascadeStatsV1 stats;
                    CssCascadeErrorV1 error;
                    if (!require(
                            cascade_css_author_rules_v1(
                                sheet,
                                node,
                                CssCascadeConfigV1{},
                                &result,
                                &stats,
                                &error),
                            "every precedence matrix cascade must succeed")) {
                        return false;
                    }

                    const CssCascadeWinnerV1* winner =
                        find_winner(sheet, result, "color");
                    if (!require(
                            winner != nullptr,
                            "every precedence matrix case must emit a color winner")) {
                        return false;
                    }

                    const bool expect_second =
                        second_wins(
                            first_profile,
                            first_important,
                            second_profile,
                            second_important);
                    const std::size_t expected_profile =
                        expect_second
                            ? second_profile
                            : first_profile;
                    const bool expected_important =
                        expect_second
                            ? second_important
                            : first_important;
                    const std::uint64_t expected_source_order =
                        expect_second ? 1U : 0U;
                    const std::string_view expected_value =
                        expect_second ? "green" : "red";

                    if (!require(
                            sheet.resolve(winner->value) ==
                                expected_value,
                            "winner value must match frozen cascade order")) {
                        return false;
                    }
                    if (!require(
                            winner->important ==
                                expected_important,
                            "winner importance flag must match frozen cascade order")) {
                        return false;
                    }
                    if (!require(
                            winner->specificity ==
                                kProfiles[expected_profile].specificity,
                            "winner specificity must match frozen selector profile")) {
                        return false;
                    }
                    if (!require(
                            winner->source_order ==
                                expected_source_order,
                            "winner source order must identify exact winning declaration")) {
                        return false;
                    }
                    if (!require(
                            result.winners.size() == 1U &&
                                stats.rules_considered == 2U &&
                                stats.matched_rules == 2U &&
                                stats.declarations_considered == 2U &&
                                stats.properties_emitted == 1U,
                            "matrix cascade counters must be exact")) {
                        return false;
                    }

                    const bool importance_replacement =
                        !first_important && second_important;
                    const bool specificity_replacement =
                        first_important == second_important &&
                        second_profile > first_profile;
                    const bool source_order_replacement =
                        first_important == second_important &&
                        second_profile == first_profile;

                    if (!require(
                            stats.wins_by_importance ==
                                (importance_replacement ? 1U : 0U),
                            "importance replacement counter must be exact")) {
                        return false;
                    }
                    if (!require(
                            stats.wins_by_specificity ==
                                (specificity_replacement ? 1U : 0U),
                            "specificity replacement counter must be exact")) {
                        return false;
                    }
                    if (!require(
                            stats.wins_by_source_order ==
                                (source_order_replacement ? 1U : 0U),
                            "source-order replacement counter must be exact")) {
                        return false;
                    }

                    ++decisions;
                    if (expect_second) {
                        ++second_wins_count;
                    } else {
                        ++first_wins;
                    }
                    importance_replacements +=
                        importance_replacement ? 1U : 0U;
                    specificity_replacements +=
                        specificity_replacement ? 1U : 0U;
                    source_order_replacements +=
                        source_order_replacement ? 1U : 0U;
                }
            }
        }
    }

    if (!require(
            decisions == kDecisionDenominator,
            "precedence matrix denominator must be exactly 64")) {
        return false;
    }
    if (!require(
            first_wins + second_wins_count ==
                kDecisionDenominator,
            "every precedence case must classify one winner")) {
        return false;
    }
    if (!require(
            importance_replacements == 16U,
            "matrix must contain exactly 16 importance replacements")) {
        return false;
    }
    if (!require(
            specificity_replacements == 12U,
            "matrix must contain exactly 12 specificity replacements")) {
        return false;
    }
    if (!require(
            source_order_replacements == 8U,
            "matrix must contain exactly eight source-order replacements")) {
        return false;
    }

    result.release();
    return require(
        ledger.accounting_clean(),
        "precedence matrix release must leave ComputedStyle ledger clean");
}

bool run_bound_and_atomicity_authority() {
    std::pmr::monotonic_buffer_resource sheet_memory;
    CssStylesheetV1 stable_sheet(&sheet_memory);
    CssStylesheetV1 two_rule_sheet(&sheet_memory);
    CssStylesheetV1 two_property_sheet(&sheet_memory);
    if (!require(
            parse_sheet("div{color:green;}", &stable_sheet),
            "stable authority sheet must parse") ||
        !require(
            parse_sheet(
                "div{color:red;}div{color:green;}",
                &two_rule_sheet),
            "two-rule authority sheet must parse") ||
        !require(
            parse_sheet(
                "div{color:green;width:2px;}",
                &two_property_sheet),
            "two-property authority sheet must parse")) {
        return false;
    }

    ResourceLedger ledger;
    ledger.set_hard_limit(
        ResourceClass::ComputedStyle,
        1U * 1024U * 1024U);
    LedgerMemoryResource memory(
        ledger,
        ResourceClass::ComputedStyle);
    CssCascadeResultV1 result(&memory);
    CssCascadeStatsV1 stats;
    CssCascadeErrorV1 error;
    const CssSelectorNodeV1 node{"div", {}};

    if (!require(
            cascade_css_author_rules_v1(
                stable_sheet,
                node,
                CssCascadeConfigV1{},
                &result,
                &stats,
                &error),
            "atomic baseline cascade must succeed")) {
        return false;
    }
    const auto stable_size = result.winners.size();
    const CssCascadeWinnerV1 stable_winner =
        result.winners.front();

    CssCascadeConfigV1 config;
    config.maximum_rules = 1U;
    if (!require(
            !cascade_css_author_rules_v1(
                two_rule_sheet,
                node,
                config,
                &result,
                &stats,
                &error),
            "rule limit must fail closed") ||
        !require(
            error.kind ==
                CssCascadeErrorKindV1::RuleLimitExceeded,
            "rule limit must report exact error") ||
        !require(
            result.winners.size() == stable_size &&
                result.winners.front() == stable_winner,
            "rule-limit failure must preserve previous output")) {
        return false;
    }

    config = CssCascadeConfigV1{};
    config.maximum_declarations = 1U;
    if (!require(
            !cascade_css_author_rules_v1(
                two_rule_sheet,
                node,
                config,
                &result,
                &stats,
                &error),
            "declaration limit must fail closed") ||
        !require(
            error.kind ==
                CssCascadeErrorKindV1::DeclarationLimitExceeded,
            "declaration limit must report exact error") ||
        !require(
            result.winners.size() == stable_size &&
                result.winners.front() == stable_winner,
            "declaration-limit failure must preserve previous output")) {
        return false;
    }

    config = CssCascadeConfigV1{};
    config.maximum_properties = 1U;
    if (!require(
            !cascade_css_author_rules_v1(
                two_property_sheet,
                node,
                config,
                &result,
                &stats,
                &error),
            "property limit must fail closed") ||
        !require(
            error.kind ==
                CssCascadeErrorKindV1::PropertyLimitExceeded,
            "property limit must report exact error") ||
        !require(
            result.winners.size() == stable_size &&
                result.winners.front() == stable_winner,
            "property-limit failure must preserve previous output")) {
        return false;
    }

    config = CssCascadeConfigV1{};
    config.maximum_work_units = 1U;
    if (!require(
            !cascade_css_author_rules_v1(
                stable_sheet,
                node,
                config,
                &result,
                &stats,
                &error),
            "tiny cascade work budget must fail closed") ||
        !require(
            error.kind ==
                CssCascadeErrorKindV1::WorkBudgetExceeded,
            "tiny cascade work budget must report exact error") ||
        !require(
            result.winners.size() == stable_size &&
                result.winners.front() == stable_winner,
            "work-budget failure must preserve previous output")) {
        return false;
    }

    result.release();
    return require(
        ledger.accounting_clean(),
        "bound authority release must leave ComputedStyle ledger clean");
}

} // namespace

int main() {
    if (!run_precedence_matrix() ||
        !run_bound_and_atomicity_authority()) {
        return 1;
    }

    std::cout
        << "{\"schema\":\"zevryon.z3-cascade-authority.v1\","
        << "\"specificity_profiles\":" << kProfileCount << ','
        << "\"importance_states\":" << kImportanceStates << ','
        << "\"decisions\":" << kDecisionDenominator
        << "}\n";
    return 0;
}
