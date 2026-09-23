#include "css_semantic_style_bridge_v1.hpp"

#include <algorithm>
#include <memory_resource>
#include <new>
#include <string_view>

namespace zevryon::style {
namespace {

bool set_error(
    CssSemanticStyleBridgeErrorV1* error,
    CssSemanticStyleBridgeErrorKindV1 kind,
    std::size_t node_index,
    std::uint64_t document_ordinal,
    CssCascadeErrorKindV1 cascade_kind,
    CssStyleDagErrorKindV1 style_dag_kind,
    std::string_view message) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->node_index = node_index;
        error->document_ordinal = document_ordinal;
        error->cascade_kind = cascade_kind;
        error->style_dag_kind = style_dag_kind;
        try {
            error->message.assign(message.data(), message.size());
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool consume_preflight_work(
    CssSemanticStyleBridgeConfigV1 config,
    CssSemanticStyleBridgeStatsV1* stats,
    std::uint64_t units) noexcept {
    if (stats == nullptr ||
        stats->work_units > config.maximum_work_units ||
        units > config.maximum_work_units - stats->work_units) {
        return false;
    }
    stats->preflight_work_units += units;
    stats->work_units += units;
    return true;
}

bool charge_nested_work(
    CssSemanticStyleBridgeConfigV1 config,
    CssSemanticStyleBridgeStatsV1* stats,
    std::uint64_t units,
    bool cascade) noexcept {
    if (stats == nullptr ||
        stats->work_units > config.maximum_work_units ||
        units > config.maximum_work_units - stats->work_units) {
        return false;
    }
    if (cascade) {
        stats->cascade_work_units += units;
    } else {
        stats->style_dag_work_units += units;
    }
    stats->work_units += units;
    return true;
}

bool charge_inline_work(
    CssSemanticStyleBridgeConfigV1 config,
    CssSemanticStyleBridgeStatsV1* stats,
    std::uint64_t units,
    bool parsing) noexcept {
    if (stats == nullptr ||
        stats->work_units > config.maximum_work_units ||
        units > config.maximum_work_units - stats->work_units) {
        return false;
    }
    if (parsing) {
        stats->inline_parse_work_units += units;
    } else {
        stats->inline_merge_work_units += units;
    }
    stats->work_units += units;
    return true;
}

bool add_semantic_bytes(
    std::size_t value,
    std::size_t node_limit,
    std::size_t total_limit,
    std::size_t* node_bytes,
    std::size_t* total_bytes) noexcept {
    if (node_bytes == nullptr || total_bytes == nullptr ||
        *node_bytes > node_limit ||
        value > node_limit - *node_bytes ||
        *total_bytes > total_limit ||
        value > total_limit - *total_bytes) {
        return false;
    }
    *node_bytes += value;
    *total_bytes += value;
    return true;
}

} // namespace

bool CssSemanticStyleBridgeConfigV1::valid() const noexcept {
    return maximum_nodes > 0U &&
        maximum_nodes <= kMaximumNodesLimit &&
        maximum_attributes_per_node > 0U &&
        maximum_attributes_per_node <=
            kMaximumAttributesPerNodeLimit &&
        maximum_total_attributes >= maximum_attributes_per_node &&
        maximum_total_attributes <= kMaximumTotalAttributesLimit &&
        maximum_node_semantic_bytes > 0U &&
        maximum_node_semantic_bytes <=
            kMaximumNodeSemanticBytesLimit &&
        maximum_total_semantic_bytes >= maximum_node_semantic_bytes &&
        maximum_total_semantic_bytes <=
            kMaximumTotalSemanticBytesLimit &&
        maximum_work_units > 0U &&
        maximum_work_units <= kMaximumWorkUnitsLimit &&
        cascade.valid() &&
        inline_parser.valid() &&
        inline_merge.valid() &&
        style_dag.valid() &&
        maximum_attributes_per_node <=
            cascade.selector_match.maximum_attributes &&
        maximum_node_semantic_bytes <=
            cascade.selector_match.maximum_semantic_bytes;
}

CssSemanticStyleWindowV1::CssSemanticStyleWindowV1(
    std::pmr::memory_resource* memory)
    : terminal_nodes(memory) {}

std::pmr::memory_resource*
CssSemanticStyleWindowV1::resource() const noexcept {
    return terminal_nodes.get_allocator().resource();
}

void CssSemanticStyleWindowV1::release() noexcept {
    std::pmr::vector<std::uint32_t> empty(resource());
    terminal_nodes.swap(empty);
    document_begin = 0U;
    document_end = 0U;
    document_node_count = 0U;
}

const char* css_semantic_style_bridge_error_kind_name_v1(
    CssSemanticStyleBridgeErrorKindV1 kind) noexcept {
    switch (kind) {
    case CssSemanticStyleBridgeErrorKindV1::None:
        return "none";
    case CssSemanticStyleBridgeErrorKindV1::InvalidConfiguration:
        return "invalid-configuration";
    case CssSemanticStyleBridgeErrorKindV1::InvalidWindow:
        return "invalid-window";
    case CssSemanticStyleBridgeErrorKindV1::NodeLimitExceeded:
        return "node-limit-exceeded";
    case CssSemanticStyleBridgeErrorKindV1::AttributeLimitExceeded:
        return "attribute-limit-exceeded";
    case CssSemanticStyleBridgeErrorKindV1::SemanticBudgetExceeded:
        return "semantic-budget-exceeded";
    case CssSemanticStyleBridgeErrorKindV1::InlineStyleParseFailure:
        return "inline-style-parse-failure";
    case CssSemanticStyleBridgeErrorKindV1::InlineStyleAtRuleUnsupported:
        return "inline-style-at-rule-unsupported";
    case CssSemanticStyleBridgeErrorKindV1::InlineCascadeMergeFailure:
        return "inline-cascade-merge-failure";
    case CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded:
        return "work-budget-exceeded";
    case CssSemanticStyleBridgeErrorKindV1::CascadeFailure:
        return "cascade-failure";
    case CssSemanticStyleBridgeErrorKindV1::StyleDagFailure:
        return "style-dag-failure";
    case CssSemanticStyleBridgeErrorKindV1::AllocationFailure:
        return "allocation-failure";
    }
    return "unknown";
}

bool compute_css_style_terminals_for_semantic_window_v1(
    const CssStylesheetV1& stylesheet,
    const zevryon::massivedoc::ZenithSemanticNodeWindowResult& window,
    CssSemanticStyleBridgeConfigV1 config,
    CssComputedStyleDagV1* dag,
    CssSemanticStyleWindowV1* output,
    CssSemanticStyleBridgeStatsV1* stats,
    CssSemanticStyleBridgeErrorV1* error) noexcept {
    if (stats != nullptr) {
        *stats = CssSemanticStyleBridgeStatsV1{};
    }
    if (error != nullptr) {
        *error = CssSemanticStyleBridgeErrorV1{};
    }

    if (dag == nullptr || output == nullptr || stats == nullptr ||
        error == nullptr || dag->resource() == nullptr ||
        output->resource() == nullptr || !config.valid()) {
        return set_error(
            error,
            CssSemanticStyleBridgeErrorKindV1::InvalidConfiguration,
            0U,
            window.start_ordinal,
            CssCascadeErrorKindV1::None,
            CssStyleDagErrorKindV1::None,
            "CSS semantic style bridge requires DAG, output, stats, error and valid configuration");
    }

    CssSemanticStyleBridgeStatsV1 candidate_stats;
    if (window.start_ordinal > window.next_ordinal ||
        window.next_ordinal > window.arena_node_count ||
        window.next_ordinal - window.start_ordinal !=
            static_cast<std::uint64_t>(window.nodes.size())) {
        return set_error(
            error,
            CssSemanticStyleBridgeErrorKindV1::InvalidWindow,
            0U,
            window.start_ordinal,
            CssCascadeErrorKindV1::None,
            CssStyleDagErrorKindV1::None,
            "Z8 semantic style window ordinal range is inconsistent with node payload");
    }
    if (window.nodes.size() > config.maximum_nodes) {
        return set_error(
            error,
            CssSemanticStyleBridgeErrorKindV1::NodeLimitExceeded,
            config.maximum_nodes,
            window.start_ordinal,
            CssCascadeErrorKindV1::None,
            CssStyleDagErrorKindV1::None,
            "Z8 semantic style window exceeds configured CSS bridge node limit");
    }

    std::size_t total_attributes = 0U;
    std::size_t total_semantic_bytes = 0U;
    for (std::size_t index = 0U;
         index < window.nodes.size();
         ++index) {
        const auto& node = window.nodes[index];
        const std::uint64_t document_ordinal =
            window.start_ordinal +
            static_cast<std::uint64_t>(index);
        ++candidate_stats.nodes_considered;
        if (!consume_preflight_work(config, &candidate_stats, 1U)) {
            *stats = candidate_stats;
            return set_error(
                error,
                CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
                index,
                document_ordinal,
                CssCascadeErrorKindV1::None,
                CssStyleDagErrorKindV1::None,
                "CSS semantic style bridge preflight node scan exceeded work budget");
        }
        if (node.attributes.size() >
                config.maximum_attributes_per_node ||
            node.record.attribute_count != node.attributes.size()) {
            *stats = candidate_stats;
            return set_error(
                error,
                node.record.attribute_count != node.attributes.size()
                    ? CssSemanticStyleBridgeErrorKindV1::InvalidWindow
                    : CssSemanticStyleBridgeErrorKindV1::AttributeLimitExceeded,
                index,
                document_ordinal,
                CssCascadeErrorKindV1::None,
                CssStyleDagErrorKindV1::None,
                "semantic node attribute payload exceeds bounds or disagrees with authoritative record count");
        }
        if (total_attributes >
                config.maximum_total_attributes ||
            node.attributes.size() >
                config.maximum_total_attributes - total_attributes) {
            *stats = candidate_stats;
            return set_error(
                error,
                CssSemanticStyleBridgeErrorKindV1::AttributeLimitExceeded,
                index,
                document_ordinal,
                CssCascadeErrorKindV1::None,
                CssStyleDagErrorKindV1::None,
                "semantic style window exceeds configured total attribute limit");
        }
        total_attributes += node.attributes.size();

        std::size_t node_semantic_bytes = 0U;
        std::size_t style_attribute_count = 0U;
        bool style_attribute_matches = false;
        if (!add_semantic_bytes(
                node.style.size(),
                config.maximum_node_semantic_bytes,
                config.maximum_total_semantic_bytes,
                &node_semantic_bytes,
                &total_semantic_bytes)) {
            *stats = candidate_stats;
            return set_error(
                error,
                CssSemanticStyleBridgeErrorKindV1::SemanticBudgetExceeded,
                index,
                document_ordinal,
                CssCascadeErrorKindV1::None,
                CssStyleDagErrorKindV1::None,
                "semantic node inline-style field exceeds CSS bridge semantic-byte budget");
        }
        if (!add_semantic_bytes(
                node.tag.size(),
                config.maximum_node_semantic_bytes,
                config.maximum_total_semantic_bytes,
                &node_semantic_bytes,
                &total_semantic_bytes)) {
            *stats = candidate_stats;
            return set_error(
                error,
                CssSemanticStyleBridgeErrorKindV1::SemanticBudgetExceeded,
                index,
                document_ordinal,
                CssCascadeErrorKindV1::None,
                CssStyleDagErrorKindV1::None,
                "semantic node tag exceeds CSS bridge semantic-byte budget");
        }
        for (const auto& attribute : node.attributes) {
            ++candidate_stats.attributes_considered;
            if (attribute.name == "style") {
                ++style_attribute_count;
                style_attribute_matches =
                    style_attribute_matches ||
                    attribute.value == node.style;
            }
            if (!consume_preflight_work(
                    config,
                    &candidate_stats,
                    1U)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
                    index,
                    document_ordinal,
                    CssCascadeErrorKindV1::None,
                    CssStyleDagErrorKindV1::None,
                    "CSS semantic style bridge attribute scan exceeded work budget");
            }
            if (!add_semantic_bytes(
                    attribute.name.size(),
                    config.maximum_node_semantic_bytes,
                    config.maximum_total_semantic_bytes,
                    &node_semantic_bytes,
                    &total_semantic_bytes) ||
                !add_semantic_bytes(
                    attribute.value.size(),
                    config.maximum_node_semantic_bytes,
                    config.maximum_total_semantic_bytes,
                    &node_semantic_bytes,
                    &total_semantic_bytes)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssSemanticStyleBridgeErrorKindV1::SemanticBudgetExceeded,
                    index,
                    document_ordinal,
                    CssCascadeErrorKindV1::None,
                    CssStyleDagErrorKindV1::None,
                    "semantic node attributes exceed CSS bridge semantic-byte budget");
            }
        }
        const bool style_payload_valid =
            style_attribute_count <= 1U &&
            (style_attribute_count == 0U
                ? node.style.empty()
                : style_attribute_matches);
        if (!style_payload_valid) {
            *stats = candidate_stats;
            return set_error(
                error,
                CssSemanticStyleBridgeErrorKindV1::InvalidWindow,
                index,
                document_ordinal,
                CssCascadeErrorKindV1::None,
                CssStyleDagErrorKindV1::None,
                "semantic node style field disagrees with retained style attribute payload");
        }
        if (node.style.size() >
            config.inline_parser.maximum_input_bytes) {
            *stats = candidate_stats;
            error->parser_kind = CssParserV1ErrorKind::InputTooLarge;
            return set_error(
                error,
                CssSemanticStyleBridgeErrorKindV1::InlineStyleParseFailure,
                index,
                document_ordinal,
                CssCascadeErrorKindV1::None,
                CssStyleDagErrorKindV1::None,
                "semantic node inline style exceeds configured declaration-list parser input bound");
        }
    }

    if (total_attributes != window.attribute_count) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssSemanticStyleBridgeErrorKindV1::InvalidWindow,
            0U,
            window.start_ordinal,
            CssCascadeErrorKindV1::None,
            CssStyleDagErrorKindV1::None,
            "Z8 semantic style window total attribute count disagrees with payload");
    }
    candidate_stats.semantic_bytes =
        static_cast<std::uint64_t>(total_semantic_bytes);

    CssSemanticStyleWindowV1 candidate(output->resource());
    candidate.document_begin = window.start_ordinal;
    candidate.document_end = window.next_ordinal;
    candidate.document_node_count = window.arena_node_count;

    try {
        candidate.terminal_nodes.reserve(window.nodes.size());

        for (std::size_t index = 0U;
             index < window.nodes.size();
             ++index) {
            const auto& node = window.nodes[index];
            const std::uint64_t document_ordinal =
                window.start_ordinal +
                static_cast<std::uint64_t>(index);

            std::pmr::monotonic_buffer_resource scratch;
            std::pmr::vector<CssSelectorAttributeV1> attributes(&scratch);
            attributes.reserve(node.attributes.size());
            for (const auto& attribute : node.attributes) {
                attributes.push_back(
                    CssSelectorAttributeV1{
                        attribute.name,
                        attribute.value});
            }
            const CssSelectorNodeV1 selector_node{
                node.tag,
                attributes};

            const std::uint64_t cascade_remaining =
                config.maximum_work_units -
                candidate_stats.work_units;
            if (cascade_remaining == 0U) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
                    index,
                    document_ordinal,
                    CssCascadeErrorKindV1::None,
                    CssStyleDagErrorKindV1::None,
                    "CSS semantic style bridge exhausted work budget before cascade");
            }
            CssCascadeConfigV1 cascade_config = config.cascade;
            cascade_config.maximum_work_units =
                std::min(
                    cascade_config.maximum_work_units,
                    cascade_remaining);

            CssCascadeResultV1 cascade_result(&scratch);
            CssCascadeStatsV1 cascade_stats;
            CssCascadeErrorV1 cascade_error;
            if (!cascade_css_author_rules_v1(
                    stylesheet,
                    selector_node,
                    cascade_config,
                    &cascade_result,
                    &cascade_stats,
                    &cascade_error)) {
                if (!charge_nested_work(
                        config,
                        &candidate_stats,
                        cascade_stats.work_units,
                        true)) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
                        index,
                        document_ordinal,
                        cascade_error.kind,
                        CssStyleDagErrorKindV1::None,
                        "CSS semantic style bridge cascade work accounting exceeded shared budget");
                }
                *stats = candidate_stats;
                const auto kind =
                    cascade_error.kind ==
                            CssCascadeErrorKindV1::WorkBudgetExceeded
                        ? CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded
                        : cascade_error.kind ==
                                  CssCascadeErrorKindV1::AllocationFailure
                            ? CssSemanticStyleBridgeErrorKindV1::AllocationFailure
                            : CssSemanticStyleBridgeErrorKindV1::CascadeFailure;
                return set_error(
                    error,
                    kind,
                    index,
                    document_ordinal,
                    cascade_error.kind,
                    CssStyleDagErrorKindV1::None,
                    cascade_error.message);
            }
            if (!charge_nested_work(
                    config,
                    &candidate_stats,
                    cascade_stats.work_units,
                    true)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
                    index,
                    document_ordinal,
                    CssCascadeErrorKindV1::None,
                    CssStyleDagErrorKindV1::None,
                    "CSS semantic style bridge cascade work exceeded shared budget");
            }

            const CssStylesheetV1* effective_stylesheet =
                &stylesheet;
            const CssCascadeResultV1* effective_cascade =
                &cascade_result;
            CssStylesheetV1 inline_stylesheet(&scratch);
            CssInlineMergedCascadeV1 merged_cascade(&scratch);

            if (!node.style.empty()) {
                const std::uint64_t parse_units =
                    static_cast<std::uint64_t>(node.style.size()) + 1U;
                if (!charge_inline_work(
                        config,
                        &candidate_stats,
                        parse_units,
                        true)) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
                        index,
                        document_ordinal,
                        CssCascadeErrorKindV1::None,
                        CssStyleDagErrorKindV1::None,
                        "CSS semantic style bridge exhausted shared work budget before inline parsing");
                }

                CssParserV1Stats parser_stats;
                CssParserV1Error parser_error;
                if (!parse_css_declaration_list_v1(
                        node.style,
                        config.inline_parser,
                        &inline_stylesheet,
                        &parser_stats,
                        &parser_error)) {
                    *stats = candidate_stats;
                    error->parser_kind = parser_error.kind;
                    const auto kind =
                        parser_error.kind ==
                                CssParserV1ErrorKind::AllocationFailure
                            ? CssSemanticStyleBridgeErrorKindV1::AllocationFailure
                            : CssSemanticStyleBridgeErrorKindV1::InlineStyleParseFailure;
                    return set_error(
                        error,
                        kind,
                        index,
                        document_ordinal,
                        CssCascadeErrorKindV1::None,
                        CssStyleDagErrorKindV1::None,
                        parser_error.message);
                }

                ++candidate_stats.inline_styles_parsed;
                candidate_stats.inline_declarations +=
                    parser_stats.declarations;
                if (!inline_stylesheet.at_rules.empty()) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssSemanticStyleBridgeErrorKindV1::InlineStyleAtRuleUnsupported,
                        index,
                        document_ordinal,
                        CssCascadeErrorKindV1::None,
                        CssStyleDagErrorKindV1::None,
                        "HTML style attribute declaration list contains an at-rule outside the supported inline profile");
                }

                const std::uint64_t merge_remaining =
                    config.maximum_work_units -
                    candidate_stats.work_units;
                if (merge_remaining == 0U) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
                        index,
                        document_ordinal,
                        CssCascadeErrorKindV1::None,
                        CssStyleDagErrorKindV1::None,
                        "CSS semantic style bridge exhausted shared work budget before inline cascade merge");
                }

                CssInlineCascadeMergeConfigV1 merge_config =
                    config.inline_merge;
                merge_config.maximum_work_units =
                    std::min(
                        merge_config.maximum_work_units,
                        merge_remaining);
                CssInlineCascadeMergeStatsV1 merge_stats;
                CssInlineCascadeMergeErrorV1 merge_error;
                if (!merge_css_author_and_inline_cascade_v1(
                        stylesheet,
                        cascade_result,
                        inline_stylesheet,
                        inline_stylesheet.declarations,
                        merge_config,
                        &merged_cascade,
                        &merge_stats,
                        &merge_error)) {
                    if (!charge_inline_work(
                            config,
                            &candidate_stats,
                            merge_stats.work_units,
                            false)) {
                        *stats = candidate_stats;
                        return set_error(
                            error,
                            CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
                            index,
                            document_ordinal,
                            CssCascadeErrorKindV1::None,
                            CssStyleDagErrorKindV1::None,
                            "CSS semantic style bridge inline merge work accounting exceeded shared budget");
                    }
                    *stats = candidate_stats;
                    error->inline_merge_kind = merge_error.kind;
                    const auto kind =
                        merge_error.kind ==
                                CssInlineCascadeMergeErrorKindV1::WorkBudgetExceeded
                            ? CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded
                            : merge_error.kind ==
                                      CssInlineCascadeMergeErrorKindV1::AllocationFailure
                                ? CssSemanticStyleBridgeErrorKindV1::AllocationFailure
                                : CssSemanticStyleBridgeErrorKindV1::InlineCascadeMergeFailure;
                    return set_error(
                        error,
                        kind,
                        index,
                        document_ordinal,
                        CssCascadeErrorKindV1::None,
                        CssStyleDagErrorKindV1::None,
                        merge_error.message);
                }
                if (!charge_inline_work(
                        config,
                        &candidate_stats,
                        merge_stats.work_units,
                        false)) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
                        index,
                        document_ordinal,
                        CssCascadeErrorKindV1::None,
                        CssStyleDagErrorKindV1::None,
                        "CSS semantic style bridge inline merge work exceeded shared budget");
                }

                effective_stylesheet =
                    &merged_cascade.stylesheet;
                effective_cascade =
                    &merged_cascade.cascade;
            }

            const std::uint64_t dag_remaining =
                config.maximum_work_units -
                candidate_stats.work_units;
            if (dag_remaining == 0U) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
                    index,
                    document_ordinal,
                    CssCascadeErrorKindV1::None,
                    CssStyleDagErrorKindV1::None,
                    "CSS semantic style bridge exhausted work budget before style interning");
            }
            CssStyleDagConfigV1 dag_config = config.style_dag;
            dag_config.maximum_work_units =
                std::min(
                    dag_config.maximum_work_units,
                    dag_remaining);

            std::uint32_t terminal = kCssStyleDagNoNodeV1;
            CssStyleDagStatsV1 dag_stats;
            CssStyleDagErrorV1 dag_error;
            if (!intern_css_cascade_style_v1(
                    *effective_stylesheet,
                    *effective_cascade,
                    dag_config,
                    dag,
                    &terminal,
                    &dag_stats,
                    &dag_error)) {
                if (!charge_nested_work(
                        config,
                        &candidate_stats,
                        dag_stats.work_units,
                        false)) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
                        index,
                        document_ordinal,
                        CssCascadeErrorKindV1::None,
                        dag_error.kind,
                        "CSS semantic style bridge DAG work accounting exceeded shared budget");
                }
                *stats = candidate_stats;
                const auto kind =
                    dag_error.kind ==
                            CssStyleDagErrorKindV1::WorkBudgetExceeded
                        ? CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded
                        : dag_error.kind ==
                                  CssStyleDagErrorKindV1::AllocationFailure
                            ? CssSemanticStyleBridgeErrorKindV1::AllocationFailure
                            : CssSemanticStyleBridgeErrorKindV1::StyleDagFailure;
                return set_error(
                    error,
                    kind,
                    index,
                    document_ordinal,
                    CssCascadeErrorKindV1::None,
                    dag_error.kind,
                    dag_error.message);
            }
            if (!charge_nested_work(
                    config,
                    &candidate_stats,
                    dag_stats.work_units,
                    false)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssSemanticStyleBridgeErrorKindV1::WorkBudgetExceeded,
                    index,
                    document_ordinal,
                    CssCascadeErrorKindV1::None,
                    CssStyleDagErrorKindV1::None,
                    "CSS semantic style bridge DAG work exceeded shared budget");
            }

            candidate.terminal_nodes.push_back(terminal);
            ++candidate_stats.nodes_styled;
        }
    } catch (const std::bad_alloc&) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssSemanticStyleBridgeErrorKindV1::AllocationFailure,
            static_cast<std::size_t>(candidate_stats.nodes_styled),
            window.start_ordinal + candidate_stats.nodes_styled,
            CssCascadeErrorKindV1::None,
            CssStyleDagErrorKindV1::None,
            "CSS semantic style bridge allocation failed");
    } catch (...) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssSemanticStyleBridgeErrorKindV1::AllocationFailure,
            static_cast<std::size_t>(candidate_stats.nodes_styled),
            window.start_ordinal + candidate_stats.nodes_styled,
            CssCascadeErrorKindV1::None,
            CssStyleDagErrorKindV1::None,
            "CSS semantic style bridge failed while allocating bounded working state");
    }

    output->document_begin = candidate.document_begin;
    output->document_end = candidate.document_end;
    output->document_node_count = candidate.document_node_count;
    output->terminal_nodes.swap(candidate.terminal_nodes);
    *stats = candidate_stats;
    return true;
}

} // namespace zevryon::style
