#pragma once

#include "css_parser_v1.hpp"
#include "css_selector_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::style {

struct CssCascadeWinnerV1 {
    CssTextSliceV1 property{};
    CssTextSliceV1 value{};
    bool important{false};
    CssSpecificityV1 specificity{};
    std::uint64_t source_order{0U};

    bool operator==(const CssCascadeWinnerV1&) const noexcept = default;
};

struct CssCascadeResultV1 {
    explicit CssCascadeResultV1(std::pmr::memory_resource* memory);

    std::pmr::vector<CssCascadeWinnerV1> winners;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;
};

struct CssCascadeConfigV1 {
    static constexpr std::size_t kMaximumRulesLimit = 1'048'576U;
    static constexpr std::size_t kMaximumDeclarationsLimit = 4'194'304U;
    static constexpr std::size_t kMaximumPropertiesLimit = 1'048'576U;
    static constexpr std::uint64_t kMaximumWorkUnitsLimit =
        64U * 1024U * 1024U;

    std::size_t maximum_rules{65'536U};
    std::size_t maximum_declarations{262'144U};
    std::size_t maximum_properties{65'536U};
    std::uint64_t maximum_work_units{8U * 1024U * 1024U};
    CssSelectorCompileConfigV1 selector_compile{};
    CssSelectorMatchConfigV1 selector_match{};

    bool valid() const noexcept;
};

enum class CssCascadeErrorKindV1 : std::uint8_t {
    None = 0,
    InvalidConfiguration,
    RuleLimitExceeded,
    DeclarationLimitExceeded,
    PropertyLimitExceeded,
    WorkBudgetExceeded,
    InvalidStylesheet,
    SelectorCompileFailure,
    SelectorMatchFailure,
    AllocationFailure,
};

struct CssCascadeErrorV1 {
    CssCascadeErrorKindV1 kind{CssCascadeErrorKindV1::None};
    std::size_t rule_index{0U};
    std::size_t declaration_index{0U};
    std::string message;
};

struct CssCascadeStatsV1 {
    std::uint64_t rules_considered{0U};
    std::uint64_t matched_rules{0U};
    std::uint64_t declarations_considered{0U};
    std::uint64_t properties_emitted{0U};
    std::uint64_t wins_by_importance{0U};
    std::uint64_t wins_by_specificity{0U};
    std::uint64_t wins_by_source_order{0U};
    std::uint64_t work_units{0U};
};

const char* css_cascade_error_kind_name_v1(
    CssCascadeErrorKindV1 kind) noexcept;

// Evaluates one bounded author-origin stylesheet against one semantic node.
// Winner declaration slices borrow CssStylesheetV1::text and therefore remain
// valid only while the stylesheet text remains unchanged.
bool cascade_css_author_rules_v1(
    const CssStylesheetV1& stylesheet,
    const CssSelectorNodeV1& node,
    CssCascadeConfigV1 config,
    CssCascadeResultV1* output,
    CssCascadeStatsV1* stats,
    CssCascadeErrorV1* error) noexcept;

} // namespace zevryon::style
