#include "css_selector_invalidation_v1.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

namespace {

using zevryon::core::LedgerMemoryResource;
using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;
using namespace zevryon::style;

constexpr std::uint32_t kTag = 1U << 0U;
constexpr std::uint32_t kId = 1U << 1U;
constexpr std::uint32_t kClass = 1U << 2U;
constexpr std::uint32_t kAttrA = 1U << 3U;
constexpr std::uint32_t kAttrB = 1U << 4U;
constexpr std::size_t kSelectorProfiles = 32U;
constexpr std::size_t kMutationsPerProfile = 8U;
constexpr std::size_t kDecisionDenominator =
    kSelectorProfiles * kMutationsPerProfile;

struct MutationCase {
    bool tag_changed{false};
    std::array<std::string_view, 2> names{};
    std::size_t name_count{0U};
    std::uint32_t dependency_mask{0U};
};

constexpr std::array<MutationCase, kMutationsPerProfile> kMutations{{
    {true, {}, 0U, kTag},
    {false, {"id", {}}, 1U, kId},
    {false, {"class", {}}, 1U, kClass},
    {false, {"data-a", {}}, 1U, kAttrA},
    {false, {"DaTa-B", {}}, 1U, kAttrB},
    {false, {"title", {}}, 1U, 0U},
    {false, {"id", "title"}, 2U, kId},
    {false, {"data-b", "data-c"}, 2U, kAttrB},
}};

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: Z3 selector invalidation authority: "
                  << message << '\n';
        return false;
    }
    return true;
}

std::string selector_source(std::uint32_t mask) {
    if (mask == 0U) {
        return "*";
    }
    std::string source;
    if ((mask & kTag) != 0U) {
        source += "div";
    }
    if ((mask & kId) != 0U) {
        source += "#hero";
    }
    if ((mask & kClass) != 0U) {
        source += ".card";
    }
    if ((mask & kAttrA) != 0U) {
        source += "[data-a]";
    }
    if ((mask & kAttrB) != 0U) {
        source += "[DATA-B=value]";
    }
    return source;
}

bool compile_selector(
    std::string_view source,
    CssCompoundSelectorV1* selector) {
    CssSelectorCompileStatsV1 stats;
    CssSelectorCompileErrorV1 error;
    return compile_css_compound_selector_v1(
        source,
        CssSelectorCompileConfigV1{},
        selector,
        &stats,
        &error);
}

bool contains_named_dependency(
    const CssSelectorDependencySetV1& dependencies,
    std::string_view name) {
    for (const CssSelectorDependencyV1& dependency :
         dependencies.dependencies) {
        if (dependency.kind ==
                CssSelectorDependencyKindV1::NamedAttribute &&
            dependencies.resolve(dependency.name) == name) {
            return true;
        }
    }
    return false;
}

bool run_exact_decision_matrix() {
    ResourceLedger ledger;
    ledger.set_hard_limit(
        ResourceClass::ComputedStyle,
        4U * 1024U * 1024U);
    LedgerMemoryResource memory(
        ledger,
        ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorDependencySetV1 dependencies(&memory);

    std::size_t decisions = 0U;
    std::size_t invalidated_true = 0U;
    std::size_t invalidated_false = 0U;

    for (std::uint32_t mask = 0U;
         mask < kSelectorProfiles;
         ++mask) {
        const std::string source = selector_source(mask);
        if (!require(
                compile_selector(source, &selector),
                "every configured selector profile must compile")) {
            return false;
        }

        CssSelectorDependencyStatsV1 build_stats;
        CssSelectorDependencyErrorV1 build_error;
        if (!require(
                build_css_selector_dependency_set_v1(
                    selector,
                    CssSelectorDependencyConfigV1{},
                    &dependencies,
                    &build_stats,
                    &build_error),
                "dependency extraction must succeed for every profile")) {
            return false;
        }

        const std::size_t expected_dependencies =
            static_cast<std::size_t>(std::popcount(mask));
        if (!require(
                dependencies.dependencies.size() ==
                    expected_dependencies,
                "dependency count must equal profile bit population")) {
            return false;
        }
        if (!require(
                build_stats.dependencies_emitted ==
                    expected_dependencies,
                "dependency emission stats must equal exact profile population")) {
            return false;
        }
        if ((mask & kAttrA) != 0U &&
            !require(
                contains_named_dependency(dependencies, "data-a"),
                "data-a dependency must be retained canonically")) {
            return false;
        }
        if ((mask & kAttrB) != 0U &&
            !require(
                contains_named_dependency(dependencies, "data-b"),
                "DATA-B dependency must canonicalize to data-b")) {
            return false;
        }

        if (!require(
                compile_selector(
                    ".replacement[data-z]",
                    &selector),
                "selector object must be replaceable after dependency extraction")) {
            return false;
        }

        for (const MutationCase& mutation : kMutations) {
            const std::span<const std::string_view> names{
                mutation.names.data(),
                mutation.name_count};
            CssSelectorDependencyStatsV1 stats;
            CssSelectorDependencyErrorV1 error;
            bool invalidated = false;
            if (!require(
                    css_selector_dependencies_invalidated_v1(
                        dependencies,
                        CssSelectorSemanticChangeV1{
                            mutation.tag_changed,
                            names},
                        CssSelectorDependencyConfigV1{},
                        &stats,
                        &invalidated,
                        &error),
                    "every configured semantic mutation must evaluate")) {
                return false;
            }

            const bool expected =
                (mask & mutation.dependency_mask) != 0U;
            if (!require(
                    invalidated == expected,
                    "invalidation decision must exactly match dependency mask")) {
                return false;
            }
            ++decisions;
            if (invalidated) {
                ++invalidated_true;
            } else {
                ++invalidated_false;
            }
        }
    }

    if (!require(
            decisions == kDecisionDenominator,
            "exact invalidation decision denominator must be 256")) {
        return false;
    }
    if (!require(
            invalidated_true + invalidated_false ==
                kDecisionDenominator,
            "all authority decisions must be classified")) {
        return false;
    }
    if (!require(
            ledger.snapshot(ResourceClass::ComputedStyle)
                    .rejected_reservations == 0U,
            "decision matrix must not hit ComputedStyle ledger rejection")) {
        return false;
    }
    return require(
        ledger.within_hard_limits(),
        "decision matrix must remain within ComputedStyle ledger limit");
}

bool run_deduplication_authority() {
    ResourceLedger ledger;
    ledger.set_hard_limit(
        ResourceClass::ComputedStyle,
        1U * 1024U * 1024U);
    LedgerMemoryResource memory(
        ledger,
        ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorDependencySetV1 dependencies(&memory);

    if (!require(
            compile_selector(
                ".a.b#one#two[data-x][DATA-X=value]",
                &selector),
            "deduplication selector must compile")) {
        return false;
    }

    CssSelectorDependencyStatsV1 stats;
    CssSelectorDependencyErrorV1 error;
    if (!require(
            build_css_selector_dependency_set_v1(
                selector,
                CssSelectorDependencyConfigV1{},
                &dependencies,
                &stats,
                &error),
            "deduplication dependency extraction must succeed")) {
        return false;
    }

    if (!require(
            dependencies.dependencies.size() == 3U,
            "class ID and repeated named attribute must collapse to three dependencies")) {
        return false;
    }
    return require(
        stats.dependencies_deduplicated == 3U,
        "deduplication stats must report three collapsed selector dependencies");
}

bool run_bound_rejection_authority() {
    ResourceLedger ledger;
    ledger.set_hard_limit(
        ResourceClass::ComputedStyle,
        1U * 1024U * 1024U);
    LedgerMemoryResource memory(
        ledger,
        ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorDependencySetV1 dependencies(&memory);

    if (!require(
            compile_selector(
                "div#hero.card[data-a][data-b]",
                &selector),
            "five-dependency limit selector must compile")) {
        return false;
    }

    CssSelectorDependencyConfigV1 config;
    config.maximum_dependencies = 4U;
    CssSelectorDependencyStatsV1 stats;
    CssSelectorDependencyErrorV1 error;
    if (!require(
            !build_css_selector_dependency_set_v1(
                selector,
                config,
                &dependencies,
                &stats,
                &error),
            "five dependencies must fail a four-dependency ceiling")) {
        return false;
    }
    if (!require(
            error.kind ==
                CssSelectorDependencyErrorKindV1::DependencyLimitExceeded,
            "dependency ceiling must report dependency-limit-exceeded")) {
        return false;
    }

    config = CssSelectorDependencyConfigV1{};
    if (!require(
            build_css_selector_dependency_set_v1(
                selector,
                config,
                &dependencies,
                &stats,
                &error),
            "baseline dependency set must build after limit rejection")) {
        return false;
    }

    const std::array<std::string_view, 1> changed{{"title"}};
    config.maximum_work_units = 1U;
    bool invalidated = true;
    if (!require(
            !css_selector_dependencies_invalidated_v1(
                dependencies,
                CssSelectorSemanticChangeV1{false, changed},
                config,
                &stats,
                &invalidated,
                &error),
            "tiny invalidation work budget must fail closed")) {
        return false;
    }
    if (!require(
            error.kind ==
                CssSelectorDependencyErrorKindV1::WorkBudgetExceeded,
            "tiny work budget must report work-budget-exceeded")) {
        return false;
    }
    return require(
        !invalidated,
        "failed invalidation must not publish a positive decision");
}

} // namespace

int main() {
    if (!run_exact_decision_matrix() ||
        !run_deduplication_authority() ||
        !run_bound_rejection_authority()) {
        return 1;
    }

    std::cout
        << "{\"schema\":\"zevryon.z3-selector-invalidation-authority.v1\","
        << "\"selector_profiles\":" << kSelectorProfiles << ','
        << "\"mutations_per_profile\":" << kMutationsPerProfile << ','
        << "\"decisions\":" << kDecisionDenominator
        << "}\n";
    return 0;
}
