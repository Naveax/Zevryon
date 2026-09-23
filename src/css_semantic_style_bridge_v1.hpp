#pragma once

#include "css_inline_cascade_merge_v1.hpp"
#include "css_parser_v1.hpp"
#include "css_style_dag_v1.hpp"
#include "zenith_semantic_node_window.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

namespace zevryon::style {

struct CssSemanticStyleBridgeConfigV1 {
    static constexpr std::size_t kMaximumNodesLimit = 65'536U;
    static constexpr std::size_t kMaximumAttributesPerNodeLimit = 65'536U;
    static constexpr std::size_t kMaximumTotalAttributesLimit = 1'048'576U;
    static constexpr std::size_t kMaximumNodeSemanticBytesLimit =
        16U * 1024U * 1024U;
    static constexpr std::size_t kMaximumTotalSemanticBytesLimit =
        256U * 1024U * 1024U;
    static constexpr std::uint64_t kMaximumWorkUnitsLimit =
        128U * 1024U * 1024U;

    std::size_t maximum_nodes{4096U};
    std::size_t maximum_attributes_per_node{4096U};
    std::size_t maximum_total_attributes{65'536U};
    std::size_t maximum_node_semantic_bytes{1U * 1024U * 1024U};
    std::size_t maximum_total_semantic_bytes{16U * 1024U * 1024U};
    std::uint64_t maximum_work_units{16U * 1024U * 1024U};
    CssCascadeConfigV1 cascade{};
    CssParserV1Config inline_parser{};
    CssInlineCascadeMergeConfigV1 inline_merge{};
    CssStyleDagConfigV1 style_dag{};

    bool valid() const noexcept;
};

enum class CssSemanticStyleBridgeErrorKindV1 : std::uint8_t {
    None = 0,
    InvalidConfiguration,
    InvalidWindow,
    NodeLimitExceeded,
    AttributeLimitExceeded,
    SemanticBudgetExceeded,
    InlineStyleParseFailure,
    InlineStyleAtRuleUnsupported,
    InlineCascadeMergeFailure,
    WorkBudgetExceeded,
    CascadeFailure,
    StyleDagFailure,
    AllocationFailure,
};

struct CssSemanticStyleBridgeErrorV1 {
    CssSemanticStyleBridgeErrorKindV1 kind{
        CssSemanticStyleBridgeErrorKindV1::None};
    std::size_t node_index{0U};
    std::uint64_t document_ordinal{0U};
    CssCascadeErrorKindV1 cascade_kind{CssCascadeErrorKindV1::None};
    CssParserV1ErrorKind parser_kind{CssParserV1ErrorKind::None};
    CssInlineCascadeMergeErrorKindV1 inline_merge_kind{
        CssInlineCascadeMergeErrorKindV1::None};
    CssStyleDagErrorKindV1 style_dag_kind{
        CssStyleDagErrorKindV1::None};
    std::string message;
};

struct CssSemanticStyleBridgeStatsV1 {
    std::uint64_t nodes_considered{0U};
    std::uint64_t nodes_styled{0U};
    std::uint64_t attributes_considered{0U};
    std::uint64_t semantic_bytes{0U};
    std::uint64_t inline_styles_parsed{0U};
    std::uint64_t inline_declarations{0U};
    std::uint64_t preflight_work_units{0U};
    std::uint64_t inline_parse_work_units{0U};
    std::uint64_t cascade_work_units{0U};
    std::uint64_t inline_merge_work_units{0U};
    std::uint64_t style_dag_work_units{0U};
    std::uint64_t work_units{0U};
};

struct CssSemanticStyleWindowV1 {
    explicit CssSemanticStyleWindowV1(
        std::pmr::memory_resource* memory);

    std::uint64_t document_begin{0U};
    std::uint64_t document_end{0U};
    std::uint64_t document_node_count{0U};
    std::pmr::vector<std::uint32_t> terminal_nodes;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;
};

const char* css_semantic_style_bridge_error_kind_name_v1(
    CssSemanticStyleBridgeErrorKindV1 kind) noexcept;

// Computes bounded author-origin style-DAG terminal identities for one already
// materialized Z8 semantic-node window.
//
// The semantic window remains the storage-facing authority. This bridge never
// opens or scans the complete logical DOM. It consumes only tag + original
// attributes from the bounded window and publishes one terminal identity per
// node.
//
// Non-empty HTML style="" payloads are parsed through the native standalone
// declaration-list parser, merged with the already-resolved author stylesheet
// cascade using inline-origin precedence, then interned into the same bounded
// style DAG. The semantic style field must agree exactly with the retained
// style attribute payload. Declaration-list at-rules are rejected at this HTML
// style-attribute boundary rather than being silently reinterpreted.
bool compute_css_style_terminals_for_semantic_window_v1(
    const CssStylesheetV1& stylesheet,
    const zevryon::massivedoc::ZenithSemanticNodeWindowResult& window,
    CssSemanticStyleBridgeConfigV1 config,
    CssComputedStyleDagV1* dag,
    CssSemanticStyleWindowV1* output,
    CssSemanticStyleBridgeStatsV1* stats,
    CssSemanticStyleBridgeErrorV1* error) noexcept;

} // namespace zevryon::style
