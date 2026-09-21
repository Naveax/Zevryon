#pragma once

#include "css_style_dag_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::style {

struct CssMaterializedPropertyV1 {
    CssTextSliceV1 property{};
    CssTextSliceV1 value{};

    bool operator==(const CssMaterializedPropertyV1&) const noexcept = default;
};

struct CssMaterializedStyleV1 {
    std::uint32_t terminal_node{0U};
    std::uint32_t property_offset{0U};
    std::uint32_t property_count{0U};

    bool operator==(const CssMaterializedStyleV1&) const noexcept = default;
};

struct CssStyleMaterializationBatchV1 {
    explicit CssStyleMaterializationBatchV1(
        std::pmr::memory_resource* memory);

    std::pmr::string text;
    std::pmr::vector<CssMaterializedPropertyV1> properties;
    std::pmr::vector<CssMaterializedStyleV1> styles;
    std::pmr::vector<std::uint32_t> request_style_indices;

    std::pmr::memory_resource* resource() const noexcept;
    std::string_view resolve(CssTextSliceV1 slice) const noexcept;
    void release() noexcept;
};

struct CssStyleMaterializationConfigV1 {
    static constexpr std::size_t kMaximumRequestsLimit = 65'536U;
    static constexpr std::size_t kMaximumUniqueStylesLimit = 65'536U;
    static constexpr std::size_t kMaximumPropertiesLimit = 1'048'576U;
    static constexpr std::size_t kMaximumTextBytesLimit =
        128U * 1024U * 1024U;
    static constexpr std::size_t kMaximumPropertiesPerStyleLimit = 65'536U;
    static constexpr std::uint64_t kMaximumWorkUnitsLimit =
        64U * 1024U * 1024U;

    std::size_t maximum_requests{4096U};
    std::size_t maximum_unique_styles{4096U};
    std::size_t maximum_properties{65'536U};
    std::size_t maximum_text_bytes{8U * 1024U * 1024U};
    std::size_t maximum_properties_per_style{4096U};
    std::uint64_t maximum_work_units{8U * 1024U * 1024U};

    bool valid() const noexcept;
};

enum class CssStyleMaterializationErrorKindV1 : std::uint8_t {
    None = 0,
    InvalidConfiguration,
    RequestLimitExceeded,
    UniqueStyleLimitExceeded,
    PropertyLimitExceeded,
    TextBudgetExceeded,
    PropertiesPerStyleExceeded,
    WorkBudgetExceeded,
    InvalidTerminalNode,
    CorruptDag,
    RepresentationOverflow,
    AllocationFailure,
};

struct CssStyleMaterializationErrorV1 {
    CssStyleMaterializationErrorKindV1 kind{
        CssStyleMaterializationErrorKindV1::None};
    std::size_t request_index{0U};
    std::string message;
};

struct CssStyleMaterializationStatsV1 {
    std::uint64_t requests{0U};
    std::uint64_t unique_styles{0U};
    std::uint64_t duplicate_style_requests{0U};
    std::uint64_t properties_materialized{0U};
    std::uint64_t text_bytes_materialized{0U};
    std::uint64_t chain_nodes_visited{0U};
    std::uint64_t duplicate_lookup_comparisons{0U};
    std::uint64_t work_units{0U};
};

const char* css_style_materialization_error_kind_name_v1(
    CssStyleMaterializationErrorKindV1 kind) noexcept;

// Materializes only the explicitly requested terminal style nodes.
//
// Each unique terminal style is flattened once. Repeated requests map to the
// same materialized style record through request_style_indices. Output owns its
// property/value text and therefore remains valid after the source DAG changes
// or is released.
bool materialize_css_style_nodes_v1(
    const CssComputedStyleDagV1& dag,
    std::span<const std::uint32_t> terminal_nodes,
    CssStyleMaterializationConfigV1 config,
    CssStyleMaterializationBatchV1* output,
    CssStyleMaterializationStatsV1* stats,
    CssStyleMaterializationErrorV1* error) noexcept;

} // namespace zevryon::style
