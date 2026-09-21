#pragma once

#include "css_cascade_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::style {

inline constexpr std::uint32_t kCssStyleDagNoNodeV1 =
    ~std::uint32_t{0};

struct CssStyleDagNodeV1 {
    std::uint32_t parent{kCssStyleDagNoNodeV1};
    CssTextSliceV1 property{};
    CssTextSliceV1 value{};
    std::uint32_t depth{0U};

    bool operator==(const CssStyleDagNodeV1&) const noexcept = default;
};

struct CssComputedStyleDagV1 {
    explicit CssComputedStyleDagV1(std::pmr::memory_resource* memory);

    std::pmr::string text;
    std::pmr::vector<CssStyleDagNodeV1> nodes;

    std::pmr::memory_resource* resource() const noexcept;
    std::string_view resolve(CssTextSliceV1 slice) const noexcept;
    void release() noexcept;
};

struct CssStyleDagConfigV1 {
    static constexpr std::size_t kMaximumPropertiesPerStyleLimit = 65'536U;
    static constexpr std::size_t kMaximumNodesLimit = 1'048'576U;
    static constexpr std::size_t kMaximumTextBytesLimit =
        128U * 1024U * 1024U;
    static constexpr std::size_t kMaximumStyleSemanticBytesLimit =
        16U * 1024U * 1024U;
    static constexpr std::uint64_t kMaximumWorkUnitsLimit =
        64U * 1024U * 1024U;

    std::size_t maximum_properties_per_style{4096U};
    std::size_t maximum_nodes{65'536U};
    std::size_t maximum_text_bytes{8U * 1024U * 1024U};
    std::size_t maximum_style_semantic_bytes{1U * 1024U * 1024U};
    std::uint64_t maximum_work_units{8U * 1024U * 1024U};

    bool valid() const noexcept;
};

enum class CssStyleDagErrorKindV1 : std::uint8_t {
    None = 0,
    InvalidConfiguration,
    PropertyLimitExceeded,
    NodeLimitExceeded,
    TextBudgetExceeded,
    SemanticBudgetExceeded,
    WorkBudgetExceeded,
    InvalidStylesheet,
    InvalidCascadeResult,
    CorruptDag,
    RepresentationOverflow,
    AllocationFailure,
};

struct CssStyleDagErrorV1 {
    CssStyleDagErrorKindV1 kind{CssStyleDagErrorKindV1::None};
    std::size_t index{0U};
    std::string message;
};

struct CssStyleDagStatsV1 {
    std::uint64_t properties_considered{0U};
    std::uint64_t nodes_created{0U};
    std::uint64_t nodes_reused{0U};
    std::uint64_t retained_validation_comparisons{0U};
    std::uint64_t canonical_order_comparisons{0U};
    std::uint64_t child_lookup_nodes_scanned{0U};
    std::uint64_t child_lookup_comparisons{0U};
    std::uint64_t text_bytes_appended{0U};
    std::uint64_t work_units{0U};
};

const char* css_style_dag_error_kind_name_v1(
    CssStyleDagErrorKindV1 kind) noexcept;

// Interns the winning property/value set into a persistent canonical prefix
// DAG. Cascade provenance (important/specificity/source-order) is intentionally
// excluded from style identity after winner selection.
//
// Node 0 is the empty root. Each later node represents its parent's style plus
// one canonical property/value assignment. Equal style prefixes reuse the same
// node; equal full winner sets therefore return the same terminal node index.
bool intern_css_cascade_style_v1(
    const CssStylesheetV1& stylesheet,
    const CssCascadeResultV1& cascade,
    CssStyleDagConfigV1 config,
    CssComputedStyleDagV1* dag,
    std::uint32_t* terminal_node,
    CssStyleDagStatsV1* stats,
    CssStyleDagErrorV1* error) noexcept;

} // namespace zevryon::style
