#include "css_cascade_v1.hpp"

#include <limits>
#include <new>
#include <utility>

namespace zevryon::style {
namespace {

bool set_error(
    CssCascadeErrorV1* error,
    CssCascadeErrorKindV1 kind,
    std::size_t rule_index,
    std::size_t declaration_index,
    const char* message) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->rule_index = rule_index;
        error->declaration_index = declaration_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool slice_valid(
    const CssStylesheetV1& stylesheet,
    CssTextSliceV1 slice) noexcept {
    const std::size_t offset = static_cast<std::size_t>(slice.offset);
    const std::size_t length = static_cast<std::size_t>(slice.length);
    return offset <= stylesheet.text.size() &&
        length <= stylesheet.text.size() - offset;
}

bool consume_work(
    CssCascadeConfigV1 config,
    CssCascadeStatsV1* stats,
    std::uint64_t units) noexcept {
    if (stats == nullptr ||
        stats->work_units > config.maximum_work_units ||
        units > config.maximum_work_units - stats->work_units) {
        return false;
    }
    stats->work_units += units;
    return true;
}

bool estimate_node_semantic_bytes(
    const CssSelectorNodeV1& node,
    CssSelectorMatchConfigV1 config,
    std::uint64_t* semantic_bytes) noexcept {
    if (semantic_bytes == nullptr ||
        node.attributes.size() > config.maximum_attributes) {
        return false;
    }

    std::uint64_t total =
        static_cast<std::uint64_t>(node.tag.size());
    if (total > config.maximum_semantic_bytes) {
        return false;
    }
    for (const CssSelectorAttributeV1& attribute : node.attributes) {
        const std::uint64_t name_bytes =
            static_cast<std::uint64_t>(attribute.name.size());
        const std::uint64_t value_bytes =
            static_cast<std::uint64_t>(attribute.value.size());
        const std::uint64_t limit =
            static_cast<std::uint64_t>(config.maximum_semantic_bytes);
        if (name_bytes > limit - total) {
            return false;
        }
        total += name_bytes;
        if (value_bytes > limit - total) {
            return false;
        }
        total += value_bytes;
    }
    *semantic_bytes = total;
    return true;
}

bool charge_selector_match_work(
    const CssCompoundSelectorV1& selector,
    const CssSelectorNodeV1& node,
    std::uint64_t node_semantic_bytes,
    CssCascadeConfigV1 config,
    CssCascadeStatsV1* stats) noexcept {
    std::uint64_t attribute_searches = 0U;
    for (const CssSelectorSimpleV1& simple : selector.simple) {
        switch (simple.kind) {
        case CssSelectorSimpleKindV1::Id:
        case CssSelectorSimpleKindV1::Class:
        case CssSelectorSimpleKindV1::AttributeExists:
        case CssSelectorSimpleKindV1::AttributeEquals:
            ++attribute_searches;
            break;
        case CssSelectorSimpleKindV1::Universal:
        case CssSelectorSimpleKindV1::Type:
            break;
        default:
            return false;
        }
    }

    const std::uint64_t simple_units =
        static_cast<std::uint64_t>(selector.simple.size());
    const std::uint64_t attribute_units =
        static_cast<std::uint64_t>(node.attributes.size());
    if (!consume_work(config, stats, simple_units) ||
        !consume_work(config, stats, node_semantic_bytes) ||
        !consume_work(config, stats, attribute_units)) {
        return false;
    }

    if (attribute_searches == 0U) {
        return true;
    }
    if (node_semantic_bytes >
        std::numeric_limits<std::uint64_t>::max() - attribute_units) {
        return false;
    }
    const std::uint64_t scan_units =
        node_semantic_bytes + attribute_units;
    if (scan_units >
        (config.maximum_work_units - stats->work_units) /
            attribute_searches) {
        return false;
    }
    return consume_work(
        config,
        stats,
        scan_units * attribute_searches);
}

bool declaration_wins(
    const CssCascadeWinnerV1& current,
    const CssDeclarationV1& candidate,
    CssSpecificityV1 specificity,
    std::uint64_t source_order,
    CssCascadeStatsV1* stats) noexcept {
    if (candidate.important != current.important) {
        if (candidate.important) {
            ++stats->wins_by_importance;
            return true;
        }
        return false;
    }

    const int specificity_order =
        compare_css_specificity_v1(specificity, current.specificity);
    if (specificity_order != 0) {
        if (specificity_order > 0) {
            ++stats->wins_by_specificity;
            return true;
        }
        return false;
    }

    if (source_order >= current.source_order) {
        ++stats->wins_by_source_order;
        return true;
    }
    return false;
}

} // namespace

CssCascadeResultV1::CssCascadeResultV1(
    std::pmr::memory_resource* memory)
    : winners(memory) {}

std::pmr::memory_resource*
CssCascadeResultV1::resource() const noexcept {
    return winners.get_allocator().resource();
}

void CssCascadeResultV1::release() noexcept {
    std::pmr::vector<CssCascadeWinnerV1> empty(resource());
    winners.swap(empty);
}

bool CssCascadeConfigV1::valid() const noexcept {
    return maximum_rules > 0U &&
        maximum_rules <= kMaximumRulesLimit &&
        maximum_declarations > 0U &&
        maximum_declarations <= kMaximumDeclarationsLimit &&
        maximum_properties > 0U &&
        maximum_properties <= kMaximumPropertiesLimit &&
        maximum_work_units > 0U &&
        maximum_work_units <= kMaximumWorkUnitsLimit &&
        selector_compile.valid() &&
        selector_match.valid();
}

const char* css_cascade_error_kind_name_v1(
    CssCascadeErrorKindV1 kind) noexcept {
    switch (kind) {
    case CssCascadeErrorKindV1::None:
        return "none";
    case CssCascadeErrorKindV1::InvalidConfiguration:
        return "invalid-configuration";
    case CssCascadeErrorKindV1::RuleLimitExceeded:
        return "rule-limit-exceeded";
    case CssCascadeErrorKindV1::DeclarationLimitExceeded:
        return "declaration-limit-exceeded";
    case CssCascadeErrorKindV1::PropertyLimitExceeded:
        return "property-limit-exceeded";
    case CssCascadeErrorKindV1::WorkBudgetExceeded:
        return "work-budget-exceeded";
    case CssCascadeErrorKindV1::InvalidStylesheet:
        return "invalid-stylesheet";
    case CssCascadeErrorKindV1::SelectorCompileFailure:
        return "selector-compile-failure";
    case CssCascadeErrorKindV1::SelectorMatchFailure:
        return "selector-match-failure";
    case CssCascadeErrorKindV1::AllocationFailure:
        return "allocation-failure";
    }
    return "unknown";
}

bool cascade_css_author_rules_v1(
    const CssStylesheetV1& stylesheet,
    const CssSelectorNodeV1& node,
    CssCascadeConfigV1 config,
    CssCascadeResultV1* output,
    CssCascadeStatsV1* stats,
    CssCascadeErrorV1* error) noexcept {
    if (stats != nullptr) {
        *stats = CssCascadeStatsV1{};
    }
    if (error != nullptr) {
        error->kind = CssCascadeErrorKindV1::None;
        error->rule_index = 0U;
        error->declaration_index = 0U;
        error->message.clear();
    }

    if (output == nullptr || stats == nullptr || error == nullptr ||
        output->resource() == nullptr || !config.valid()) {
        return set_error(
            error,
            CssCascadeErrorKindV1::InvalidConfiguration,
            0U,
            0U,
            "CSS cascade output, stats, error and valid configuration are required");
    }
    if (stylesheet.rules.size() > config.maximum_rules) {
        return set_error(
            error,
            CssCascadeErrorKindV1::RuleLimitExceeded,
            config.maximum_rules,
            0U,
            "CSS cascade rule count exceeds configured limit");
    }
    if (stylesheet.declarations.size() > config.maximum_declarations) {
        return set_error(
            error,
            CssCascadeErrorKindV1::DeclarationLimitExceeded,
            0U,
            config.maximum_declarations,
            "CSS cascade declaration count exceeds configured limit");
    }

    CssCascadeResultV1 candidate(output->resource());
    CssCascadeStatsV1 candidate_stats;
    CssCompoundSelectorV1 selector(output->resource());
    CssSelectorCompileStatsV1 selector_stats;
    CssSelectorCompileErrorV1 selector_error;
    std::uint64_t node_semantic_bytes = 0U;
    if (!estimate_node_semantic_bytes(
            node,
            config.selector_match,
            &node_semantic_bytes)) {
        return set_error(
            error,
            CssCascadeErrorKindV1::SelectorMatchFailure,
            0U,
            0U,
            "CSS cascade semantic node exceeds selector match bounds");
    }

    try {
        for (std::size_t rule_index = 0U;
             rule_index < stylesheet.rules.size();
             ++rule_index) {
            const CssStyleRuleV1& rule = stylesheet.rules[rule_index];
            ++candidate_stats.rules_considered;

            if (!slice_valid(stylesheet, rule.selector)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssCascadeErrorKindV1::InvalidStylesheet,
                    rule_index,
                    0U,
                    "CSS cascade rule selector slice is invalid");
            }

            const std::size_t declaration_offset =
                static_cast<std::size_t>(rule.declaration_offset);
            const std::size_t declaration_count =
                static_cast<std::size_t>(rule.declaration_count);
            if (declaration_offset > stylesheet.declarations.size() ||
                declaration_count >
                    stylesheet.declarations.size() - declaration_offset) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssCascadeErrorKindV1::InvalidStylesheet,
                    rule_index,
                    declaration_offset,
                    "CSS cascade rule declaration range is invalid");
            }

            const std::string_view selector_text =
                stylesheet.resolve(rule.selector);
            if (!consume_work(
                    config,
                    &candidate_stats,
                    static_cast<std::uint64_t>(selector_text.size()) + 1U)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssCascadeErrorKindV1::WorkBudgetExceeded,
                    rule_index,
                    declaration_offset,
                    "CSS cascade selector work budget exceeded");
            }

            if (!compile_css_compound_selector_v1(
                    selector_text,
                    config.selector_compile,
                    &selector,
                    &selector_stats,
                    &selector_error)) {
                *stats = candidate_stats;
                if (selector_error.kind ==
                    CssSelectorCompileErrorKindV1::AllocationFailure) {
                    return set_error(
                        error,
                        CssCascadeErrorKindV1::AllocationFailure,
                        rule_index,
                        declaration_offset,
                        "CSS cascade selector allocation was rejected");
                }
                return set_error(
                    error,
                    CssCascadeErrorKindV1::SelectorCompileFailure,
                    rule_index,
                    declaration_offset,
                    "CSS cascade encountered selector syntax outside the supported foundation");
            }

            if (!charge_selector_match_work(
                    selector,
                    node,
                    node_semantic_bytes,
                    config,
                    &candidate_stats)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssCascadeErrorKindV1::WorkBudgetExceeded,
                    rule_index,
                    declaration_offset,
                    "CSS cascade aggregate selector match work budget exceeded");
            }

            bool matched = false;
            if (!match_css_compound_selector_v1(
                    selector,
                    node,
                    config.selector_match,
                    &matched)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssCascadeErrorKindV1::SelectorMatchFailure,
                    rule_index,
                    declaration_offset,
                    "CSS cascade selector match failed closed");
            }
            if (!matched) {
                continue;
            }
            ++candidate_stats.matched_rules;

            for (std::size_t local_index = 0U;
                 local_index < declaration_count;
                 ++local_index) {
                const std::size_t declaration_index =
                    declaration_offset + local_index;
                const CssDeclarationV1& declaration =
                    stylesheet.declarations[declaration_index];
                ++candidate_stats.declarations_considered;

                if (!slice_valid(stylesheet, declaration.property) ||
                    !slice_valid(stylesheet, declaration.value)) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssCascadeErrorKindV1::InvalidStylesheet,
                        rule_index,
                        declaration_index,
                        "CSS cascade declaration slice is invalid");
                }

                const std::string_view property =
                    stylesheet.resolve(declaration.property);
                CssCascadeWinnerV1* existing = nullptr;
                for (CssCascadeWinnerV1& winner : candidate.winners) {
                    const std::string_view existing_property =
                        stylesheet.resolve(winner.property);
                    const std::uint64_t units =
                        static_cast<std::uint64_t>(property.size()) +
                        static_cast<std::uint64_t>(existing_property.size()) +
                        1U;
                    if (!consume_work(config, &candidate_stats, units)) {
                        *stats = candidate_stats;
                        return set_error(
                            error,
                            CssCascadeErrorKindV1::WorkBudgetExceeded,
                            rule_index,
                            declaration_index,
                            "CSS cascade property lookup work budget exceeded");
                    }
                    if (property == existing_property) {
                        existing = &winner;
                        break;
                    }
                }

                const std::uint64_t source_order =
                    static_cast<std::uint64_t>(declaration_index);
                if (existing == nullptr) {
                    if (candidate.winners.size() >=
                        config.maximum_properties) {
                        *stats = candidate_stats;
                        return set_error(
                            error,
                            CssCascadeErrorKindV1::PropertyLimitExceeded,
                            rule_index,
                            declaration_index,
                            "CSS cascade unique-property limit exceeded");
                    }
                    candidate.winners.push_back(CssCascadeWinnerV1{
                        declaration.property,
                        declaration.value,
                        declaration.important,
                        selector.specificity,
                        source_order,
                    });
                    continue;
                }

                if (declaration_wins(
                        *existing,
                        declaration,
                        selector.specificity,
                        source_order,
                        &candidate_stats)) {
                    existing->value = declaration.value;
                    existing->important = declaration.important;
                    existing->specificity = selector.specificity;
                    existing->source_order = source_order;
                }
            }
        }

        candidate_stats.properties_emitted =
            static_cast<std::uint64_t>(candidate.winners.size());
        output->release();
        output->winners.swap(candidate.winners);
        *stats = candidate_stats;
        return true;
    } catch (const std::bad_alloc&) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssCascadeErrorKindV1::AllocationFailure,
            0U,
            0U,
            "CSS cascade allocation rejected by bounded memory resource");
    } catch (...) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssCascadeErrorKindV1::AllocationFailure,
            0U,
            0U,
            "CSS cascade allocation or container operation failed");
    }
}

} // namespace zevryon::style
