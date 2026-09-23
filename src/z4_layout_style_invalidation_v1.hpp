#pragma once

#include "css_semantic_style_bridge_v1.hpp"
#include "css_style_dag_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

namespace zevryon::layout {

struct LayoutStyleInvalidationConfigV1 {
    static constexpr std::size_t kMaximumNodesLimit = 65'536U;
    static constexpr std::size_t kMaximumRangesLimit = 65'536U;
    static constexpr std::uint64_t kMaximumWorkUnitsLimit = 131'072U;

    std::size_t maximum_nodes{4096U};
    std::size_t maximum_ranges{4096U};
    std::uint64_t maximum_work_units{8192U};

    bool valid() const noexcept;
};

struct LayoutStyleInvalidationRangeV1 {
    std::uint64_t document_begin{0U};
    std::uint64_t document_end{0U};

    bool operator==(const LayoutStyleInvalidationRangeV1&) const noexcept =
        default;
};

enum class LayoutStyleInvalidationErrorKindV1 : std::uint8_t {
    None = 0,
    InvalidConfiguration,
    InvalidWindow,
    NodeLimitExceeded,
    RangeLimitExceeded,
    WorkBudgetExceeded,
    InvalidStyleTerminal,
    AllocationFailure,
};

struct LayoutStyleInvalidationErrorV1 {
    LayoutStyleInvalidationErrorKindV1 kind{
        LayoutStyleInvalidationErrorKindV1::None};
    std::size_t node_index{0U};
    std::uint64_t document_ordinal{0U};
    std::string message;
};

struct LayoutStyleInvalidationStatsV1 {
    std::uint64_t nodes_considered{0U};
    std::uint64_t changed_nodes{0U};
    std::uint64_t output_ranges{0U};
    std::uint64_t work_units{0U};
};

struct LayoutStyleInvalidationResultV1 {
    explicit LayoutStyleInvalidationResultV1(
        std::pmr::memory_resource* memory);

    std::pmr::vector<LayoutStyleInvalidationRangeV1> ranges;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;
};

const char* layout_style_invalidation_error_kind_name_v1(
    LayoutStyleInvalidationErrorKindV1 kind) noexcept;

// Computes exact changed-style ranges for two snapshots of the same bounded
// document window. Both terminal arrays must refer to the same canonical
// CssComputedStyleDagV1 identity space.
//
// This foundation deliberately does not expand changes through descendants,
// formatting contexts or layout dependencies. It is the bounded diff primitive
// that later Z4 propagation logic can consume without scanning the full DOM.
bool compute_layout_style_invalidation_v1(
    const zevryon::style::CssComputedStyleDagV1& dag,
    const zevryon::style::CssSemanticStyleWindowV1& previous,
    const zevryon::style::CssSemanticStyleWindowV1& current,
    LayoutStyleInvalidationConfigV1 config,
    LayoutStyleInvalidationResultV1* output,
    LayoutStyleInvalidationStatsV1* stats,
    LayoutStyleInvalidationErrorV1* error) noexcept;

} // namespace zevryon::layout
