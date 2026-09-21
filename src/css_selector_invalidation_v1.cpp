#include "css_selector_invalidation_v1.hpp"

#include <limits>
#include <new>
#include <utility>

namespace zevryon::style {
namespace {

char ascii_lower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

bool selector_slice_valid(
    const CssCompoundSelectorV1& selector,
    CssSelectorTextSliceV1 slice) noexcept {
    const std::size_t offset = static_cast<std::size_t>(slice.offset);
    const std::size_t length = static_cast<std::size_t>(slice.length);
    return offset <= selector.text.size() &&
        length <= selector.text.size() - offset;
}

bool dependency_slice_valid(
    const CssSelectorDependencySetV1& dependencies,
    CssSelectorTextSliceV1 slice) noexcept {
    const std::size_t offset = static_cast<std::size_t>(slice.offset);
    const std::size_t length = static_cast<std::size_t>(slice.length);
    return offset <= dependencies.text.size() &&
        length <= dependencies.text.size() - offset;
}

bool set_error(
    CssSelectorDependencyErrorV1* error,
    CssSelectorDependencyErrorKindV1 kind,
    std::size_t index,
    const char* message) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->index = index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool consume_work(
    CssSelectorDependencyConfigV1 config,
    CssSelectorDependencyStatsV1* stats,
    std::uint64_t units) noexcept {
    if (stats == nullptr ||
        stats->work_units > config.maximum_work_units ||
        units > config.maximum_work_units - stats->work_units) {
        return false;
    }
    stats->work_units += units;
    return true;
}

bool validate_simple_shape(
    const CssCompoundSelectorV1& selector,
    const CssSelectorSimpleV1& simple) noexcept {
    if (!selector_slice_valid(selector, simple.name) ||
        !selector_slice_valid(selector, simple.value)) {
        return false;
    }

    switch (simple.kind) {
    case CssSelectorSimpleKindV1::Universal:
        return simple.name.length == 0U && simple.value.length == 0U;
    case CssSelectorSimpleKindV1::Type:
    case CssSelectorSimpleKindV1::Id:
    case CssSelectorSimpleKindV1::Class:
        return simple.name.length != 0U && simple.value.length == 0U;
    case CssSelectorSimpleKindV1::AttributeExists:
        return simple.name.length != 0U && simple.value.length == 0U;
    case CssSelectorSimpleKindV1::AttributeEquals:
        return simple.name.length != 0U;
    }
    return false;
}

bool validate_dependency_shape(
    const CssSelectorDependencySetV1& dependencies,
    const CssSelectorDependencyV1& dependency) noexcept {
    switch (dependency.kind) {
    case CssSelectorDependencyKindV1::Tag:
    case CssSelectorDependencyKindV1::IdAttribute:
    case CssSelectorDependencyKindV1::ClassAttribute:
        return dependency.name.length == 0U &&
            dependency_slice_valid(dependencies, dependency.name);
    case CssSelectorDependencyKindV1::NamedAttribute:
        return dependency.name.length != 0U &&
            dependency_slice_valid(dependencies, dependency.name);
    }
    return false;
}

bool append_canonical_name(
    std::string_view name,
    CssSelectorDependencyConfigV1 config,
    CssSelectorDependencySetV1* output,
    CssSelectorTextSliceV1* slice) {
    if (output == nullptr || slice == nullptr) {
        return false;
    }
    if (output->text.size() > config.maximum_semantic_bytes ||
        name.size() > config.maximum_semantic_bytes - output->text.size() ||
        output->text.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()) ||
        name.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }

    slice->offset =
        static_cast<std::uint32_t>(output->text.size());
    slice->length = static_cast<std::uint32_t>(name.size());
    for (const char character : name) {
        output->text.push_back(ascii_lower(character));
    }
    return true;
}

} // namespace

CssSelectorDependencySetV1::CssSelectorDependencySetV1(
    std::pmr::memory_resource* memory)
    : text(memory), dependencies(memory) {}

std::pmr::memory_resource*
CssSelectorDependencySetV1::resource() const noexcept {
    return text.get_allocator().resource();
}

std::string_view CssSelectorDependencySetV1::resolve(
    CssSelectorTextSliceV1 slice) const noexcept {
    const std::size_t offset = static_cast<std::size_t>(slice.offset);
    const std::size_t length = static_cast<std::size_t>(slice.length);
    if (offset > text.size() || length > text.size() - offset) {
        return {};
    }
    return std::string_view(text.data() + offset, length);
}

void CssSelectorDependencySetV1::release() noexcept {
    std::pmr::string empty_text(resource());
    std::pmr::vector<CssSelectorDependencyV1> empty_dependencies(resource());
    text.swap(empty_text);
    dependencies.swap(empty_dependencies);
}

bool CssSelectorDependencyConfigV1::valid() const noexcept {
    return maximum_dependencies > 0U &&
        maximum_dependencies <= kMaximumDependenciesLimit &&
        maximum_changed_attributes > 0U &&
        maximum_changed_attributes <= kMaximumChangedAttributesLimit &&
        maximum_semantic_bytes > 0U &&
        maximum_semantic_bytes <= kMaximumSemanticBytesLimit &&
        maximum_work_units > 0U &&
        maximum_work_units <= kMaximumWorkUnitsLimit;
}

const char* css_selector_dependency_error_kind_name_v1(
    CssSelectorDependencyErrorKindV1 kind) noexcept {
    switch (kind) {
    case CssSelectorDependencyErrorKindV1::None:
        return "none";
    case CssSelectorDependencyErrorKindV1::InvalidConfiguration:
        return "invalid-configuration";
    case CssSelectorDependencyErrorKindV1::InvalidSelector:
        return "invalid-selector";
    case CssSelectorDependencyErrorKindV1::InvalidDependencySet:
        return "invalid-dependency-set";
    case CssSelectorDependencyErrorKindV1::InvalidChange:
        return "invalid-change";
    case CssSelectorDependencyErrorKindV1::DependencyLimitExceeded:
        return "dependency-limit-exceeded";
    case CssSelectorDependencyErrorKindV1::ChangedAttributeLimitExceeded:
        return "changed-attribute-limit-exceeded";
    case CssSelectorDependencyErrorKindV1::SemanticBudgetExceeded:
        return "semantic-budget-exceeded";
    case CssSelectorDependencyErrorKindV1::WorkBudgetExceeded:
        return "work-budget-exceeded";
    case CssSelectorDependencyErrorKindV1::AllocationFailure:
        return "allocation-failure";
    }
    return "unknown";
}

bool build_css_selector_dependency_set_v1(
    const CssCompoundSelectorV1& selector,
    CssSelectorDependencyConfigV1 config,
    CssSelectorDependencySetV1* output,
    CssSelectorDependencyStatsV1* stats,
    CssSelectorDependencyErrorV1* error) noexcept {
    if (stats != nullptr) {
        *stats = CssSelectorDependencyStatsV1{};
    }
    if (error != nullptr) {
        error->kind = CssSelectorDependencyErrorKindV1::None;
        error->index = 0U;
        error->message.clear();
    }
    if (output == nullptr || stats == nullptr || error == nullptr ||
        output->resource() == nullptr || !config.valid()) {
        return set_error(
            error,
            CssSelectorDependencyErrorKindV1::InvalidConfiguration,
            0U,
            "CSS selector dependency output, stats, error and valid configuration are required");
    }
    if (selector.simple.empty()) {
        return set_error(
            error,
            CssSelectorDependencyErrorKindV1::InvalidSelector,
            0U,
            "CSS selector dependency extraction requires a compiled selector");
    }

    CssSelectorDependencySetV1 candidate(output->resource());
    CssSelectorDependencyStatsV1 candidate_stats;

    try {
        for (std::size_t index = 0U;
             index < selector.simple.size();
             ++index) {
            const CssSelectorSimpleV1& simple = selector.simple[index];
            ++candidate_stats.simple_selectors_examined;
            if (!consume_work(config, &candidate_stats, 1U)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssSelectorDependencyErrorKindV1::WorkBudgetExceeded,
                    index,
                    "CSS selector dependency scan exceeded the work budget");
            }
            if (!validate_simple_shape(selector, simple)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssSelectorDependencyErrorKindV1::InvalidSelector,
                    index,
                    "CSS selector dependency extraction encountered a corrupt simple selector");
            }

            CssSelectorDependencyV1 dependency;
            bool emit = true;
            std::string_view named_attribute;
            switch (simple.kind) {
            case CssSelectorSimpleKindV1::Universal:
                emit = false;
                break;
            case CssSelectorSimpleKindV1::Type:
                dependency.kind = CssSelectorDependencyKindV1::Tag;
                break;
            case CssSelectorSimpleKindV1::Id:
                dependency.kind = CssSelectorDependencyKindV1::IdAttribute;
                break;
            case CssSelectorSimpleKindV1::Class:
                dependency.kind = CssSelectorDependencyKindV1::ClassAttribute;
                break;
            case CssSelectorSimpleKindV1::AttributeExists:
            case CssSelectorSimpleKindV1::AttributeEquals:
                dependency.kind =
                    CssSelectorDependencyKindV1::NamedAttribute;
                named_attribute = selector.resolve(simple.name);
                break;
            default:
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssSelectorDependencyErrorKindV1::InvalidSelector,
                    index,
                    "CSS selector dependency extraction encountered an unknown selector kind");
            }
            if (!emit) {
                continue;
            }

            bool duplicate = false;
            for (const CssSelectorDependencyV1& existing :
                 candidate.dependencies) {
                if (!consume_work(config, &candidate_stats, 1U)) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssSelectorDependencyErrorKindV1::WorkBudgetExceeded,
                        index,
                        "CSS selector dependency lookup exceeded the work budget");
                }
                if (existing.kind != dependency.kind) {
                    continue;
                }
                if (dependency.kind !=
                    CssSelectorDependencyKindV1::NamedAttribute) {
                    duplicate = true;
                    break;
                }

                const std::string_view existing_name =
                    candidate.resolve(existing.name);
                const std::uint64_t units =
                    static_cast<std::uint64_t>(existing_name.size()) +
                    static_cast<std::uint64_t>(named_attribute.size());
                if (!consume_work(
                        config,
                        &candidate_stats,
                        units)) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssSelectorDependencyErrorKindV1::WorkBudgetExceeded,
                        index,
                        "CSS selector dependency deduplication exceeded the work budget");
                }
                if (ascii_iequals(existing_name, named_attribute)) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                ++candidate_stats.dependencies_deduplicated;
                continue;
            }
            if (candidate.dependencies.size() >=
                config.maximum_dependencies) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssSelectorDependencyErrorKindV1::DependencyLimitExceeded,
                    index,
                    "CSS selector dependency count exceeds configured limit");
            }

            if (dependency.kind ==
                CssSelectorDependencyKindV1::NamedAttribute) {
                if (candidate.text.size() >
                        config.maximum_semantic_bytes ||
                    named_attribute.size() >
                        config.maximum_semantic_bytes -
                            candidate.text.size()) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssSelectorDependencyErrorKindV1::SemanticBudgetExceeded,
                        index,
                        "CSS selector dependency semantic bytes exceed configured limit");
                }
                if (!append_canonical_name(
                        named_attribute,
                        config,
                        &candidate,
                        &dependency.name)) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssSelectorDependencyErrorKindV1::SemanticBudgetExceeded,
                        index,
                        "CSS selector dependency name cannot fit retained semantic storage");
                }
            }

            candidate.dependencies.push_back(dependency);
            if (dependency.kind ==
                CssSelectorDependencyKindV1::NamedAttribute) {
                ++candidate_stats.named_attribute_dependencies;
            }
        }

        candidate_stats.dependencies_emitted =
            static_cast<std::uint64_t>(candidate.dependencies.size());
        output->release();
        output->text.swap(candidate.text);
        output->dependencies.swap(candidate.dependencies);
        *stats = candidate_stats;
        return true;
    } catch (const std::bad_alloc&) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssSelectorDependencyErrorKindV1::AllocationFailure,
            0U,
            "CSS selector dependency allocation rejected by bounded memory resource");
    } catch (...) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssSelectorDependencyErrorKindV1::AllocationFailure,
            0U,
            "CSS selector dependency allocation or container operation failed");
    }
}

bool css_selector_dependencies_invalidated_v1(
    const CssSelectorDependencySetV1& dependencies,
    const CssSelectorSemanticChangeV1& change,
    CssSelectorDependencyConfigV1 config,
    CssSelectorDependencyStatsV1* stats,
    bool* invalidated,
    CssSelectorDependencyErrorV1* error) noexcept {
    if (stats != nullptr) {
        *stats = CssSelectorDependencyStatsV1{};
    }
    if (invalidated != nullptr) {
        *invalidated = false;
    }
    if (error != nullptr) {
        error->kind = CssSelectorDependencyErrorKindV1::None;
        error->index = 0U;
        error->message.clear();
    }
    if (stats == nullptr || invalidated == nullptr || error == nullptr ||
        !config.valid()) {
        return set_error(
            error,
            CssSelectorDependencyErrorKindV1::InvalidConfiguration,
            0U,
            "CSS selector invalidation stats, output, error and valid configuration are required");
    }
    if (dependencies.dependencies.size() >
        config.maximum_dependencies) {
        return set_error(
            error,
            CssSelectorDependencyErrorKindV1::DependencyLimitExceeded,
            config.maximum_dependencies,
            "CSS selector invalidation dependency count exceeds configured limit");
    }
    if (dependencies.text.size() > config.maximum_semantic_bytes) {
        return set_error(
            error,
            CssSelectorDependencyErrorKindV1::SemanticBudgetExceeded,
            0U,
            "CSS selector invalidation retained dependency text exceeds configured limit");
    }
    if (change.changed_attribute_names.size() >
        config.maximum_changed_attributes) {
        return set_error(
            error,
            CssSelectorDependencyErrorKindV1::ChangedAttributeLimitExceeded,
            config.maximum_changed_attributes,
            "CSS selector invalidation changed-attribute count exceeds configured limit");
    }

    std::size_t semantic_bytes = 0U;
    for (std::size_t index = 0U;
         index < change.changed_attribute_names.size();
         ++index) {
        const std::string_view name =
            change.changed_attribute_names[index];
        if (!consume_work(config, stats, 1U)) {
            return set_error(
                error,
                CssSelectorDependencyErrorKindV1::WorkBudgetExceeded,
                index,
                "CSS selector invalidation change scan exceeded the work budget");
        }
        if (name.empty()) {
            return set_error(
                error,
                CssSelectorDependencyErrorKindV1::InvalidChange,
                index,
                "CSS selector invalidation change contains an empty attribute name");
        }
        if (semantic_bytes > config.maximum_semantic_bytes ||
            name.size() >
                config.maximum_semantic_bytes - semantic_bytes) {
            return set_error(
                error,
                CssSelectorDependencyErrorKindV1::SemanticBudgetExceeded,
                index,
                "CSS selector invalidation changed-attribute semantic bytes exceed configured limit");
        }
        semantic_bytes += name.size();
    }

    for (std::size_t index = 0U;
         index < dependencies.dependencies.size();
         ++index) {
        const CssSelectorDependencyV1& dependency =
            dependencies.dependencies[index];
        if (!consume_work(config, stats, 1U)) {
            return set_error(
                error,
                CssSelectorDependencyErrorKindV1::WorkBudgetExceeded,
                index,
                "CSS selector invalidation dependency scan exceeded the work budget");
        }
        if (!validate_dependency_shape(dependencies, dependency)) {
            return set_error(
                error,
                CssSelectorDependencyErrorKindV1::InvalidDependencySet,
                index,
                "CSS selector invalidation encountered a corrupt dependency record");
        }
        if (dependency.kind ==
            CssSelectorDependencyKindV1::NamedAttribute) {
            ++stats->named_attribute_dependencies;
        }
    }
    stats->dependencies_emitted =
        static_cast<std::uint64_t>(
            dependencies.dependencies.size());

    for (const CssSelectorDependencyV1& dependency :
         dependencies.dependencies) {
        if (dependency.kind == CssSelectorDependencyKindV1::Tag) {
            if (change.tag_changed) {
                *invalidated = true;
                return true;
            }
            continue;
        }

        std::string_view expected;
        switch (dependency.kind) {
        case CssSelectorDependencyKindV1::IdAttribute:
            expected = "id";
            break;
        case CssSelectorDependencyKindV1::ClassAttribute:
            expected = "class";
            break;
        case CssSelectorDependencyKindV1::NamedAttribute:
            expected = dependencies.resolve(dependency.name);
            break;
        case CssSelectorDependencyKindV1::Tag:
            break;
        default:
            return set_error(
                error,
                CssSelectorDependencyErrorKindV1::InvalidDependencySet,
                0U,
                "CSS selector invalidation encountered an unknown dependency kind");
        }

        for (const std::string_view changed :
             change.changed_attribute_names) {
            ++stats->attribute_change_comparisons;
            const std::uint64_t units =
                static_cast<std::uint64_t>(expected.size()) +
                static_cast<std::uint64_t>(changed.size());
            if (!consume_work(config, stats, units)) {
                return set_error(
                    error,
                    CssSelectorDependencyErrorKindV1::WorkBudgetExceeded,
                    0U,
                    "CSS selector invalidation attribute comparison exceeded the work budget");
            }
            if (ascii_iequals(expected, changed)) {
                *invalidated = true;
                return true;
            }
        }
    }

    return true;
}

} // namespace zevryon::style
