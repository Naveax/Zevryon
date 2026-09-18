#include "css_selector_invalidation_v1.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstddef>
#include <iostream>
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

bool compile(
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

bool build(
    const CssCompoundSelectorV1& selector,
    CssSelectorDependencySetV1* dependencies,
    CssSelectorDependencyStatsV1* stats,
    CssSelectorDependencyErrorV1* error,
    CssSelectorDependencyConfigV1 config = {}) {
    return build_css_selector_dependency_set_v1(
        selector, config, dependencies, stats, error);
}

bool invalidated(
    const CssSelectorDependencySetV1& dependencies,
    bool tag_changed,
    std::span<const std::string_view> changed_attributes,
    bool* result,
    CssSelectorDependencyStatsV1* stats,
    CssSelectorDependencyErrorV1* error,
    CssSelectorDependencyConfigV1 config = {}) {
    return css_selector_dependencies_invalidated_v1(
        dependencies,
        CssSelectorSemanticChangeV1{tag_changed, changed_attributes},
        config,
        stats,
        result,
        error);
}

std::size_t count_kind(
    const CssSelectorDependencySetV1& dependencies,
    CssSelectorDependencyKindV1 kind) {
    std::size_t count = 0U;
    for (const CssSelectorDependencyV1& dependency :
         dependencies.dependencies) {
        if (dependency.kind == kind) {
            ++count;
        }
    }
    return count;
}

bool has_named(
    const CssSelectorDependencySetV1& dependencies,
    std::string_view expected) {
    for (const CssSelectorDependencyV1& dependency :
         dependencies.dependencies) {
        if (dependency.kind ==
                CssSelectorDependencyKindV1::NamedAttribute &&
            dependencies.resolve(dependency.name) == expected) {
            return true;
        }
    }
    return false;
}

bool test_dependency_extraction() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorDependencySetV1 dependencies(&memory);
    CssSelectorDependencyStatsV1 stats;
    CssSelectorDependencyErrorV1 error;

    bool ok = expect(
        compile("DIV#hero.card[data-state=open][hidden]", &selector),
        "selector dependency fixture must compile");
    ok &= expect(
        build(selector, &dependencies, &stats, &error),
        "selector dependencies must build");
    ok &= expect(error.kind == CssSelectorDependencyErrorKindV1::None,
                 "successful dependency build must clear error");
    ok &= expect(dependencies.dependencies.size() == 5U,
                 "fixture must emit five dependency keys");
    ok &= expect(
        count_kind(dependencies, CssSelectorDependencyKindV1::Tag) == 1U,
        "type selector must emit one tag dependency");
    ok &= expect(
        count_kind(
            dependencies,
            CssSelectorDependencyKindV1::IdAttribute) == 1U,
        "ID selector must emit one id-attribute dependency");
    ok &= expect(
        count_kind(
            dependencies,
            CssSelectorDependencyKindV1::ClassAttribute) == 1U,
        "class selector must emit one class-attribute dependency");
    ok &= expect(has_named(dependencies, "data-state"),
                 "attribute equality must emit data-state dependency");
    ok &= expect(has_named(dependencies, "hidden"),
                 "attribute existence must emit hidden dependency");
    ok &= expect(stats.dependencies_emitted == 5U,
                 "dependency emission stats must be exact");
    return ok;
}

bool test_dependency_deduplication() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorDependencySetV1 dependencies(&memory);
    CssSelectorDependencyStatsV1 stats;
    CssSelectorDependencyErrorV1 error;

    bool ok = expect(
        compile(".a.b[data-x][DATA-X=ok]", &selector),
        "dedup selector must compile");
    ok &= expect(
        build(selector, &dependencies, &stats, &error),
        "dedup dependencies must build");
    ok &= expect(dependencies.dependencies.size() == 2U,
                 "class and repeated named attribute must dedup to two keys");
    ok &= expect(stats.dependencies_deduplicated == 2U,
                 "dedup stats must count duplicate class and attribute");
    ok &= expect(
        count_kind(
            dependencies,
            CssSelectorDependencyKindV1::ClassAttribute) == 1U,
        "class dependency must be unique");
    ok &= expect(has_named(dependencies, "data-x"),
                 "named attribute dedup must be ASCII-case-insensitive");
    return ok;
}

bool test_invalidation_decisions() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorDependencySetV1 dependencies(&memory);
    CssSelectorDependencyStatsV1 stats;
    CssSelectorDependencyErrorV1 error;

    bool ok = expect(
        compile("div#hero.card[data-state=open]", &selector),
        "invalidation selector must compile");
    ok &= expect(
        build(selector, &dependencies, &stats, &error),
        "invalidation dependencies must build");

    const auto check = [&](bool tag_changed,
                           std::span<const std::string_view> names,
                           bool expected,
                           std::string_view message) {
        bool value = !expected;
        CssSelectorDependencyStatsV1 local_stats;
        CssSelectorDependencyErrorV1 local_error;
        return expect(
            invalidated(
                dependencies,
                tag_changed,
                names,
                &value,
                &local_stats,
                &local_error) &&
                value == expected,
            message);
    };

    const std::array<std::string_view, 1> id_change{{"id"}};
    const std::array<std::string_view, 1> class_change{{"class"}};
    const std::array<std::string_view, 1> named_change{{"DATA-STATE"}};
    const std::array<std::string_view, 1> unrelated_change{{"title"}};
    ok &= check(true, {}, true, "tag change must invalidate type selector");
    ok &= check(false, id_change, true, "id change must invalidate ID selector");
    ok &= check(
        false,
        class_change,
        true,
        "class change must invalidate class selector");
    ok &= check(
        false,
        named_change,
        true,
        "named attribute change must match ASCII-case-insensitively");
    ok &= check(
        false,
        unrelated_change,
        false,
        "unrelated attribute change must not invalidate selector");
    return ok;
}

bool test_universal_selector_has_no_semantic_dependency() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorDependencySetV1 dependencies(&memory);
    CssSelectorDependencyStatsV1 stats;
    CssSelectorDependencyErrorV1 error;

    bool ok = expect(compile("*", &selector), "universal selector must compile");
    ok &= expect(
        build(selector, &dependencies, &stats, &error),
        "universal dependency set must build");
    ok &= expect(dependencies.dependencies.empty(),
                 "universal selector must have no semantic dependency");

    const std::array<std::string_view, 1> attr_change{{"class"}};
    bool value = true;
    ok &= expect(
        invalidated(
            dependencies,
            true,
            attr_change,
            &value,
            &stats,
            &error) &&
            !value,
        "universal selector truth must survive tag/attribute changes");
    return ok;
}

bool test_atomic_failure_and_corruption() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorDependencySetV1 dependencies(&memory);
    CssSelectorDependencyStatsV1 stats;
    CssSelectorDependencyErrorV1 error;

    bool ok = expect(
        compile(".stable", &selector),
        "atomic dependency selector must compile");
    ok &= expect(
        build(selector, &dependencies, &stats, &error),
        "atomic dependency baseline must build");
    const std::size_t stable_count = dependencies.dependencies.size();

    selector.simple.front().kind =
        static_cast<CssSelectorSimpleKindV1>(0xffU);
    ok &= expect(
        !build(selector, &dependencies, &stats, &error),
        "unknown selector kind must fail dependency extraction");
    ok &= expect(
        error.kind == CssSelectorDependencyErrorKindV1::InvalidSelector,
        "unknown selector kind must report invalid-selector");
    ok &= expect(
        dependencies.dependencies.size() == stable_count,
        "failed dependency extraction must preserve prior output");

    selector.simple.front().kind = CssSelectorSimpleKindV1::Class;
    selector.simple.front().name.offset =
        static_cast<std::uint32_t>(selector.text.size() + 1U);
    ok &= expect(
        !build(selector, &dependencies, &stats, &error),
        "corrupt selector slice must fail dependency extraction");
    ok &= expect(
        error.kind == CssSelectorDependencyErrorKindV1::InvalidSelector,
        "corrupt selector slice must report invalid-selector");

    selector.simple.front().name.offset = 0U;
    dependencies.dependencies.front().kind =
        static_cast<CssSelectorDependencyKindV1>(0xffU);
    bool value = true;
    ok &= expect(
        !invalidated(
            dependencies,
            false,
            {},
            &value,
            &stats,
            &error),
        "corrupt dependency record must fail invalidation");
    ok &= expect(
        error.kind ==
            CssSelectorDependencyErrorKindV1::InvalidDependencySet,
        "corrupt dependency record must report invalid-dependency-set");
    ok &= expect(!value, "failed invalidation must leave output false");
    return ok;
}

bool test_dependency_set_owns_named_keys() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorDependencySetV1 dependencies(&memory);
    CssSelectorDependencyStatsV1 stats;
    CssSelectorDependencyErrorV1 error;

    bool ok = expect(
        compile("[data-a]", &selector),
        "owned dependency fixture must compile");
    ok &= expect(
        build(selector, &dependencies, &stats, &error),
        "owned dependency set must build");
    ok &= expect(
        dependencies.resolve(dependencies.dependencies.front().name) ==
            "data-a",
        "dependency set must retain canonical named key");

    ok &= expect(
        compile("[data-b]", &selector),
        "selector object must be reusable after dependency extraction");

    const std::array<std::string_view, 1> old_name{{"DATA-A"}};
    bool value = false;
    ok &= expect(
        invalidated(
            dependencies,
            false,
            old_name,
            &value,
            &stats,
            &error) &&
            value,
        "dependency set must survive selector replacement");

    const std::array<std::string_view, 1> new_name{{"data-b"}};
    value = true;
    ok &= expect(
        invalidated(
            dependencies,
            false,
            new_name,
            &value,
            &stats,
            &error) &&
            !value,
        "dependency set must not adopt replacement selector names");
    return ok;
}

bool test_limits() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorDependencySetV1 dependencies(&memory);
    CssSelectorDependencyStatsV1 stats;
    CssSelectorDependencyErrorV1 error;

    bool ok = expect(
        compile("#id.class[data-x]", &selector),
        "limit selector must compile");

    CssSelectorDependencyConfigV1 config;
    config.maximum_dependencies = 1U;
    ok &= expect(
        !build(selector, &dependencies, &stats, &error, config),
        "dependency count limit must fail closed");
    ok &= expect(
        error.kind ==
            CssSelectorDependencyErrorKindV1::DependencyLimitExceeded,
        "dependency count limit must report exact error");

    config = CssSelectorDependencyConfigV1{};
    config.maximum_semantic_bytes = 4U;
    ok &= expect(
        !build(selector, &dependencies, &stats, &error, config),
        "retained named-dependency semantic-byte limit must fail closed");
    ok &= expect(
        error.kind ==
            CssSelectorDependencyErrorKindV1::SemanticBudgetExceeded,
        "retained dependency semantic-byte limit must report exact error");

    config = CssSelectorDependencyConfigV1{};
    ok &= expect(
        build(selector, &dependencies, &stats, &error, config),
        "limit baseline dependency set must build");

    const std::array<std::string_view, 2> two_changes{{"id", "class"}};
    config.maximum_changed_attributes = 1U;
    bool value = false;
    ok &= expect(
        !invalidated(
            dependencies,
            false,
            two_changes,
            &value,
            &stats,
            &error,
            config),
        "changed-attribute count limit must fail closed");
    ok &= expect(
        error.kind ==
            CssSelectorDependencyErrorKindV1::ChangedAttributeLimitExceeded,
        "changed-attribute count must report exact error");

    const std::array<std::string_view, 1> long_change{{"title"}};
    config = CssSelectorDependencyConfigV1{};
    config.maximum_semantic_bytes = 4U;
    ok &= expect(
        !invalidated(
            dependencies,
            false,
            long_change,
            &value,
            &stats,
            &error,
            config),
        "changed-attribute semantic-byte limit must fail closed");
    ok &= expect(
        error.kind ==
            CssSelectorDependencyErrorKindV1::SemanticBudgetExceeded,
        "semantic-byte limit must report exact error");

    config = CssSelectorDependencyConfigV1{};
    config.maximum_work_units = 1U;
    const std::array<std::string_view, 1> data_change{{"data-x"}};
    ok &= expect(
        !invalidated(
            dependencies,
            false,
            data_change,
            &value,
            &stats,
            &error,
            config),
        "invalidation work budget must fail closed");
    ok &= expect(
        error.kind ==
            CssSelectorDependencyErrorKindV1::WorkBudgetExceeded,
        "work budget must report exact error");

    config = CssSelectorDependencyConfigV1{};
    config.maximum_dependencies = 1U;
    ok &= expect(
        !invalidated(
            dependencies,
            false,
            {},
            &value,
            &stats,
            &error,
            config),
        "external dependency set above configured limit must fail closed");
    ok &= expect(
        error.kind ==
            CssSelectorDependencyErrorKindV1::DependencyLimitExceeded,
        "external dependency count must report exact error");
    return ok;
}

bool test_real_ledger_rejection() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U << 20U);
    LedgerMemoryResource memory(ledger, ResourceClass::ComputedStyle);
    CssCompoundSelectorV1 selector(&memory);
    CssSelectorDependencySetV1 dependencies(&memory);
    CssSelectorDependencyStatsV1 stats;
    CssSelectorDependencyErrorV1 error;

    bool ok = expect(
        compile(".a.b.c[data-x][data-y][hidden]", &selector),
        "ledger dependency selector must compile before low dependency budget");

    ResourceLedger dependency_ledger;
    dependency_ledger.set_hard_limit(ResourceClass::ComputedStyle, 1U);
    LedgerMemoryResource dependency_memory(
        dependency_ledger, ResourceClass::ComputedStyle);
    CssSelectorDependencySetV1 low_dependencies(&dependency_memory);

    ok &= expect(
        !build(selector, &low_dependencies, &stats, &error),
        "real dependency ledger rejection must fail build");
    ok &= expect(
        error.kind == CssSelectorDependencyErrorKindV1::AllocationFailure,
        "real dependency ledger rejection must report allocation-failure");
    ok &= expect(
        dependency_ledger.snapshot(ResourceClass::ComputedStyle)
                .rejected_reservations > 0U,
        "dependency ledger rejection must be visible");
    ok &= expect(
        dependency_ledger.accounting_clean(),
        "dependency ledger rejection accounting must remain clean");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_dependency_extraction();
    ok &= test_dependency_deduplication();
    ok &= test_invalidation_decisions();
    ok &= test_universal_selector_has_no_semantic_dependency();
    ok &= test_atomic_failure_and_corruption();
    ok &= test_dependency_set_owns_named_keys();
    ok &= test_limits();
    ok &= test_real_ledger_rejection();
    if (!ok) {
        return 1;
    }
    std::cout << "CSS selector invalidation v1 tests passed\n";
    return 0;
}
