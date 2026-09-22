#include "css_style_materialization_window_v1.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::core::LedgerMemoryResource;
using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;
using namespace zevryon::style;

constexpr std::uint64_t kDocumentNodes = 1'000'000U;
constexpr std::size_t kUniqueStyles = 64U;
constexpr std::uint64_t kVisibleBegin = 500'000U;
constexpr std::size_t kVisibleNodes = 1'024U;
constexpr std::size_t kLookaheadBefore = 256U;
constexpr std::size_t kLookaheadAfter = 256U;
constexpr std::size_t kSelectedRequests =
    kVisibleNodes + kLookaheadBefore + kLookaheadAfter;
constexpr std::uint64_t kExpectedSelectedBegin =
    kVisibleBegin - kLookaheadBefore;
constexpr std::uint64_t kExpectedSelectedEnd =
    kVisibleBegin + kVisibleNodes + kLookaheadAfter;
constexpr std::size_t kCandidateTerminalBytes =
    kSelectedRequests * sizeof(std::uint32_t);
constexpr std::size_t kExpectedDagNodes = 66U;
constexpr std::size_t kExpectedDagTextBytes = 366U;
constexpr std::size_t kExpectedMaterializedProperties = 128U;
constexpr std::size_t kExpectedMaterializedTextBytes = 492U;
constexpr std::size_t kExpectedDuplicateRequests =
    kSelectedRequests - kUniqueStyles;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr
            << "FAILED: Z3 offscreen materialization authority: "
            << message << '\n';
        return false;
    }
    return true;
}

bool build_style(
    std::size_t index,
    CssComputedStyleDagV1* dag,
    std::uint32_t* terminal) {
    std::pmr::monotonic_buffer_resource scratch;
    CssStylesheetV1 sheet(&scratch);
    CssCascadeResultV1 cascade(&scratch);
    CssParserV1Stats parser_stats;
    CssParserV1Error parser_error;
    CssCascadeStatsV1 cascade_stats;
    CssCascadeErrorV1 cascade_error;
    CssStyleDagStatsV1 dag_stats;
    CssStyleDagErrorV1 dag_error;

    const std::string suffix = std::to_string(index);
    const std::string source =
        "div{z" + suffix + ":v" + suffix + ";a:1;}";

    if (!parse_css_stylesheet_v1(
            source,
            CssParserV1Config{},
            &sheet,
            &parser_stats,
            &parser_error)) {
        return false;
    }
    if (!cascade_css_author_rules_v1(
            sheet,
            CssSelectorNodeV1{"div", {}},
            CssCascadeConfigV1{},
            &cascade,
            &cascade_stats,
            &cascade_error)) {
        return false;
    }
    return intern_css_cascade_style_v1(
        sheet,
        cascade,
        CssStyleDagConfigV1{},
        dag,
        terminal,
        &dag_stats,
        &dag_error);
}

bool test_million_node_bounded_window_authority() {
    ResourceLedger ledger;
    ledger.set_hard_limit(
        ResourceClass::ComputedStyle,
        16U * 1024U * 1024U);
    LedgerMemoryResource memory(
        ledger,
        ResourceClass::ComputedStyle);
    CssComputedStyleDagV1 dag(&memory);

    std::vector<std::uint32_t> terminals;
    terminals.reserve(kUniqueStyles);
    for (std::size_t index = 0U;
         index < kUniqueStyles;
         ++index) {
        std::uint32_t terminal = kCssStyleDagNoNodeV1;
        if (!require(
                build_style(index, &dag, &terminal),
                "authority style must build")) {
            return false;
        }
        terminals.push_back(terminal);
    }

    if (!require(
            dag.nodes.size() == kExpectedDagNodes,
            "64 styles must retain exact shared-prefix DAG node count")) {
        return false;
    }
    if (!require(
            dag.text.size() == kExpectedDagTextBytes,
            "64 styles must retain exact shared-prefix DAG text bytes")) {
        return false;
    }

    // Only the bounded projected candidate is resident. The million-node
    // logical document is represented by its count, not an O(document) array.
    std::array<std::uint32_t, kSelectedRequests> candidate{};
    for (std::size_t offset = 0U;
         offset < candidate.size();
         ++offset) {
        const std::uint64_t document_index =
            kExpectedSelectedBegin +
            static_cast<std::uint64_t>(offset);
        candidate[offset] =
            terminals[
                static_cast<std::size_t>(
                    document_index % terminals.size())];
    }
    static_assert(kCandidateTerminalBytes == 6'144U);

    CssStyleMaterializationBatchV1 output(&memory);
    CssStyleMaterializationWindowConfigV1 config;
    config.maximum_visible_nodes = kVisibleNodes;
    config.offscreen_before_nodes = kLookaheadBefore;
    config.offscreen_after_nodes = kLookaheadAfter;
    config.maximum_selection_work_units = kSelectedRequests;
    config.materialization.maximum_requests = kSelectedRequests;
    config.materialization.maximum_unique_styles = kUniqueStyles;
    config.materialization.maximum_properties =
        kExpectedMaterializedProperties;
    config.materialization.maximum_text_bytes = 4096U;
    config.materialization.maximum_properties_per_style = 2U;
    config.materialization.maximum_work_units = 1U << 20U;

    CssStyleMaterializationWindowStatsV1 stats;
    CssStyleMaterializationWindowErrorV1 error;
    if (!require(
            materialize_css_style_window_v1(
                dag,
                candidate,
                kExpectedSelectedBegin,
                kDocumentNodes,
                kVisibleBegin,
                kVisibleBegin + kVisibleNodes,
                config,
                &output,
                &stats,
                &error),
            "million-node bounded candidate window must materialize")) {
        return false;
    }

    if (!require(
            stats.document_nodes == kDocumentNodes &&
                stats.candidate_nodes == kSelectedRequests &&
                stats.candidate_document_begin ==
                    kExpectedSelectedBegin &&
                stats.candidate_document_end ==
                    kExpectedSelectedEnd &&
                stats.visible_nodes == kVisibleNodes &&
                stats.offscreen_before_nodes ==
                    kLookaheadBefore &&
                stats.offscreen_after_nodes ==
                    kLookaheadAfter &&
                stats.selected_requests ==
                    kSelectedRequests &&
                stats.selection_work_units ==
                    kSelectedRequests,
            "million-node bounded-input denominator must be exact")) {
        return false;
    }
    if (!require(
            stats.selected_document_begin ==
                kExpectedSelectedBegin &&
                stats.selected_document_end ==
                    kExpectedSelectedEnd,
            "million-node selected range must be exact")) {
        return false;
    }

    if (!require(
            output.request_style_indices.size() ==
                kSelectedRequests,
            "every selected document node must receive request mapping")) {
        return false;
    }
    if (!require(
            output.styles.size() == kUniqueStyles &&
                output.properties.size() ==
                    kExpectedMaterializedProperties &&
                output.text.size() ==
                    kExpectedMaterializedTextBytes,
            "materialized output denominator must be exact")) {
        return false;
    }
    if (!require(
            stats.materialization.requests ==
                kSelectedRequests &&
                stats.materialization.unique_styles ==
                    kUniqueStyles &&
                stats.materialization.duplicate_style_requests ==
                    kExpectedDuplicateRequests &&
                stats.materialization.properties_materialized ==
                    kExpectedMaterializedProperties,
            "nested materializer denominator must be exact")) {
        return false;
    }

    for (std::size_t offset = 0U;
         offset < kSelectedRequests;
         ++offset) {
        const std::uint32_t style_index =
            output.request_style_indices[offset];
        if (!require(
                static_cast<std::size_t>(style_index) <
                    output.styles.size(),
                "request mapping must point inside materialized style table")) {
            return false;
        }
        if (!require(
                output.styles[
                    static_cast<std::size_t>(style_index)]
                        .terminal_node ==
                    candidate[offset],
                "request mapping must preserve bounded candidate terminal identity")) {
            return false;
        }
    }

    output.release();
    dag.release();
    if (!require(
            ledger.accounting_clean(),
            "authority release must leave ComputedStyle ledger accounting clean")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_million_node_bounded_window_authority()) {
        return 1;
    }

    std::cout
        << "Z3 offscreen materialization authority PASS document_nodes="
        << kDocumentNodes
        << " candidate_terminal_nodes=" << kSelectedRequests
        << " candidate_terminal_bytes=" << kCandidateTerminalBytes
        << " visible=" << kVisibleNodes
        << " offscreen_before=" << kLookaheadBefore
        << " offscreen_after=" << kLookaheadAfter
        << " selected=" << kSelectedRequests
        << " unique_styles=" << kUniqueStyles
        << " properties=" << kExpectedMaterializedProperties
        << '\n';
    return 0;
}
