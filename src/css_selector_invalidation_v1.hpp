#pragma once

#include "css_selector_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::style {

enum class CssSelectorDependencyKindV1 : std::uint8_t {
    Tag = 0,
    IdAttribute,
    ClassAttribute,
    NamedAttribute,
};

struct CssSelectorDependencyV1 {
    CssSelectorDependencyKindV1 kind{CssSelectorDependencyKindV1::Tag};
    CssSelectorTextSliceV1 name{};

    bool operator==(const CssSelectorDependencyV1&) const noexcept = default;
};

struct CssSelectorDependencySetV1 {
    explicit CssSelectorDependencySetV1(std::pmr::memory_resource* memory);

    std::pmr::string text;
    std::pmr::vector<CssSelectorDependencyV1> dependencies;

    std::pmr::memory_resource* resource() const noexcept;
    std::string_view resolve(CssSelectorTextSliceV1 slice) const noexcept;
    void release() noexcept;
};

struct CssSelectorDependencyConfigV1 {
    static constexpr std::size_t kMaximumDependenciesLimit = 65'536U;
    static constexpr std::size_t kMaximumChangedAttributesLimit = 65'536U;
    static constexpr std::size_t kMaximumSemanticBytesLimit =
        16U * 1024U * 1024U;
    static constexpr std::uint64_t kMaximumWorkUnitsLimit =
        64U * 1024U * 1024U;

    std::size_t maximum_dependencies{256U};
    std::size_t maximum_changed_attributes{4096U};
    std::size_t maximum_semantic_bytes{1U * 1024U * 1024U};
    std::uint64_t maximum_work_units{8U * 1024U * 1024U};

    bool valid() const noexcept;
};

enum class CssSelectorDependencyErrorKindV1 : std::uint8_t {
    None = 0,
    InvalidConfiguration,
    InvalidSelector,
    InvalidDependencySet,
    InvalidChange,
    DependencyLimitExceeded,
    ChangedAttributeLimitExceeded,
    SemanticBudgetExceeded,
    WorkBudgetExceeded,
    AllocationFailure,
};

struct CssSelectorDependencyErrorV1 {
    CssSelectorDependencyErrorKindV1 kind{
        CssSelectorDependencyErrorKindV1::None};
    std::size_t index{0U};
    std::string message;
};

struct CssSelectorDependencyStatsV1 {
    std::uint64_t simple_selectors_examined{0U};
    std::uint64_t dependencies_emitted{0U};
    std::uint64_t dependencies_deduplicated{0U};
    std::uint64_t named_attribute_dependencies{0U};
    std::uint64_t attribute_change_comparisons{0U};
    std::uint64_t work_units{0U};
};

struct CssSelectorSemanticChangeV1 {
    bool tag_changed{false};
    std::span<const std::string_view> changed_attribute_names;
};

const char* css_selector_dependency_error_kind_name_v1(
    CssSelectorDependencyErrorKindV1 kind) noexcept;

// Builds a conservative, self-contained dependency set for one compiled
// compound selector. Named attribute keys are copied into the dependency set's
// PMR text storage, so the compiled selector may later be replaced or released.
bool build_css_selector_dependency_set_v1(
    const CssCompoundSelectorV1& selector,
    CssSelectorDependencyConfigV1 config,
    CssSelectorDependencySetV1* output,
    CssSelectorDependencyStatsV1* stats,
    CssSelectorDependencyErrorV1* error) noexcept;

// Returns true in *invalidated when the semantic mutation may change selector
// match truth. Attribute names are compared with HTML ASCII case-insensitivity.
// Attribute renames must report both the old and new names.
bool css_selector_dependencies_invalidated_v1(
    const CssSelectorDependencySetV1& dependencies,
    const CssSelectorSemanticChangeV1& change,
    CssSelectorDependencyConfigV1 config,
    CssSelectorDependencyStatsV1* stats,
    bool* invalidated,
    CssSelectorDependencyErrorV1* error) noexcept;

} // namespace zevryon::style
