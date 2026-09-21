#include "css_style_materialization_v1.hpp"

#include <limits>
#include <new>
#include <utility>

namespace zevryon::style {
namespace {

bool set_error(
    CssStyleMaterializationErrorV1* error,
    CssStyleMaterializationErrorKindV1 kind,
    std::size_t request_index,
    const char* message) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->request_index = request_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool dag_slice_valid(
    const CssComputedStyleDagV1& dag,
    CssTextSliceV1 slice) noexcept {
    const std::size_t offset = static_cast<std::size_t>(slice.offset);
    const std::size_t length = static_cast<std::size_t>(slice.length);
    return offset <= dag.text.size() &&
        length <= dag.text.size() - offset;
}

bool consume_work(
    CssStyleMaterializationConfigV1 config,
    CssStyleMaterializationStatsV1* stats,
    std::uint64_t units) noexcept {
    if (stats == nullptr ||
        stats->work_units > config.maximum_work_units ||
        units > config.maximum_work_units - stats->work_units) {
        return false;
    }
    stats->work_units += units;
    return true;
}

bool append_text(
    std::string_view value,
    CssStyleMaterializationConfigV1 config,
    CssStyleMaterializationBatchV1* output,
    CssTextSliceV1* slice,
    CssStyleMaterializationStatsV1* stats) {
    if (output == nullptr || slice == nullptr || stats == nullptr) {
        return false;
    }
    if (output->text.size() > config.maximum_text_bytes ||
        value.size() > config.maximum_text_bytes - output->text.size()) {
        return false;
    }
    if (output->text.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()) ||
        value.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    slice->offset =
        static_cast<std::uint32_t>(output->text.size());
    slice->length = static_cast<std::uint32_t>(value.size());
    output->text.append(value.data(), value.size());
    stats->text_bytes_materialized +=
        static_cast<std::uint64_t>(value.size());
    return true;
}

bool validate_root(
    const CssComputedStyleDagV1& dag,
    CssStyleMaterializationConfigV1 config,
    CssStyleMaterializationStatsV1* stats,
    CssStyleMaterializationErrorV1* error) noexcept {
    if (dag.nodes.empty()) {
        return set_error(
            error,
            CssStyleMaterializationErrorKindV1::CorruptDag,
            0U,
            "CSS style materialization requires DAG root node");
    }
    if (!consume_work(config, stats, 1U)) {
        return set_error(
            error,
            CssStyleMaterializationErrorKindV1::WorkBudgetExceeded,
            0U,
            "CSS style materialization root validation exceeded work budget");
    }
    const CssStyleDagNodeV1& root = dag.nodes.front();
    if (root.parent != kCssStyleDagNoNodeV1 ||
        root.property.length != 0U ||
        root.value.length != 0U ||
        root.depth != 0U) {
        return set_error(
            error,
            CssStyleMaterializationErrorKindV1::CorruptDag,
            0U,
            "CSS style materialization DAG root is invalid");
    }
    return true;
}

bool validate_chain_node(
    const CssComputedStyleDagV1& dag,
    std::uint32_t node_index,
    CssStyleMaterializationConfigV1 config,
    CssStyleMaterializationStatsV1* stats,
    std::size_t request_index,
    CssStyleMaterializationErrorV1* error) noexcept {
    const std::size_t index =
        static_cast<std::size_t>(node_index);
    if (index >= dag.nodes.size()) {
        return set_error(
            error,
            CssStyleMaterializationErrorKindV1::InvalidTerminalNode,
            request_index,
            "CSS style materialization node index is out of range");
    }
    if (!consume_work(config, stats, 1U)) {
        return set_error(
            error,
            CssStyleMaterializationErrorKindV1::WorkBudgetExceeded,
            request_index,
            "CSS style materialization chain traversal exceeded work budget");
    }
    ++stats->chain_nodes_visited;

    const CssStyleDagNodeV1& node = dag.nodes[index];
    if (node.parent >= node_index ||
        node.property.length == 0U ||
        !dag_slice_valid(dag, node.property) ||
        !dag_slice_valid(dag, node.value)) {
        return set_error(
            error,
            CssStyleMaterializationErrorKindV1::CorruptDag,
            request_index,
            "CSS style materialization encountered corrupt DAG node");
    }

    const CssStyleDagNodeV1& parent =
        dag.nodes[static_cast<std::size_t>(node.parent)];
    if (parent.depth == std::numeric_limits<std::uint32_t>::max() ||
        node.depth != parent.depth + 1U) {
        return set_error(
            error,
            CssStyleMaterializationErrorKindV1::CorruptDag,
            request_index,
            "CSS style materialization encountered inconsistent DAG depth");
    }

    if (node.parent != 0U) {
        if (!dag_slice_valid(dag, parent.property)) {
            return set_error(
                error,
                CssStyleMaterializationErrorKindV1::CorruptDag,
                request_index,
                "CSS style materialization parent property slice is invalid");
        }
        const std::string_view parent_property =
            dag.resolve(parent.property);
        const std::string_view property =
            dag.resolve(node.property);
        const std::uint64_t comparison_units =
            static_cast<std::uint64_t>(parent_property.size()) +
            static_cast<std::uint64_t>(property.size()) +
            1U;
        if (!consume_work(config, stats, comparison_units)) {
            return set_error(
                error,
                CssStyleMaterializationErrorKindV1::WorkBudgetExceeded,
                request_index,
                "CSS style materialization canonical-order validation exceeded work budget");
        }
        if (parent_property.compare(property) >= 0) {
            return set_error(
                error,
                CssStyleMaterializationErrorKindV1::CorruptDag,
                request_index,
                "CSS style materialization DAG property order is not canonical");
        }
    }
    return true;
}

} // namespace

CssStyleMaterializationBatchV1::CssStyleMaterializationBatchV1(
    std::pmr::memory_resource* memory)
    : text(memory),
      properties(memory),
      styles(memory),
      request_style_indices(memory) {}

std::pmr::memory_resource*
CssStyleMaterializationBatchV1::resource() const noexcept {
    return text.get_allocator().resource();
}

std::string_view CssStyleMaterializationBatchV1::resolve(
    CssTextSliceV1 slice) const noexcept {
    const std::size_t offset = static_cast<std::size_t>(slice.offset);
    const std::size_t length = static_cast<std::size_t>(slice.length);
    if (offset > text.size() || length > text.size() - offset) {
        return {};
    }
    return std::string_view(text.data() + offset, length);
}

void CssStyleMaterializationBatchV1::release() noexcept {
    std::pmr::string empty_text(resource());
    std::pmr::vector<CssMaterializedPropertyV1> empty_properties(resource());
    std::pmr::vector<CssMaterializedStyleV1> empty_styles(resource());
    std::pmr::vector<std::uint32_t> empty_requests(resource());
    text.swap(empty_text);
    properties.swap(empty_properties);
    styles.swap(empty_styles);
    request_style_indices.swap(empty_requests);
}

bool CssStyleMaterializationConfigV1::valid() const noexcept {
    return maximum_requests > 0U &&
        maximum_requests <= kMaximumRequestsLimit &&
        maximum_unique_styles > 0U &&
        maximum_unique_styles <= kMaximumUniqueStylesLimit &&
        maximum_properties > 0U &&
        maximum_properties <= kMaximumPropertiesLimit &&
        maximum_text_bytes > 0U &&
        maximum_text_bytes <= kMaximumTextBytesLimit &&
        maximum_text_bytes <=
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()) &&
        maximum_properties_per_style > 0U &&
        maximum_properties_per_style <=
            kMaximumPropertiesPerStyleLimit &&
        maximum_work_units > 0U &&
        maximum_work_units <= kMaximumWorkUnitsLimit;
}

const char* css_style_materialization_error_kind_name_v1(
    CssStyleMaterializationErrorKindV1 kind) noexcept {
    switch (kind) {
    case CssStyleMaterializationErrorKindV1::None:
        return "none";
    case CssStyleMaterializationErrorKindV1::InvalidConfiguration:
        return "invalid-configuration";
    case CssStyleMaterializationErrorKindV1::RequestLimitExceeded:
        return "request-limit-exceeded";
    case CssStyleMaterializationErrorKindV1::UniqueStyleLimitExceeded:
        return "unique-style-limit-exceeded";
    case CssStyleMaterializationErrorKindV1::PropertyLimitExceeded:
        return "property-limit-exceeded";
    case CssStyleMaterializationErrorKindV1::TextBudgetExceeded:
        return "text-budget-exceeded";
    case CssStyleMaterializationErrorKindV1::PropertiesPerStyleExceeded:
        return "properties-per-style-exceeded";
    case CssStyleMaterializationErrorKindV1::WorkBudgetExceeded:
        return "work-budget-exceeded";
    case CssStyleMaterializationErrorKindV1::InvalidTerminalNode:
        return "invalid-terminal-node";
    case CssStyleMaterializationErrorKindV1::CorruptDag:
        return "corrupt-dag";
    case CssStyleMaterializationErrorKindV1::RepresentationOverflow:
        return "representation-overflow";
    case CssStyleMaterializationErrorKindV1::AllocationFailure:
        return "allocation-failure";
    }
    return "unknown";
}

bool materialize_css_style_nodes_v1(
    const CssComputedStyleDagV1& dag,
    std::span<const std::uint32_t> terminal_nodes,
    CssStyleMaterializationConfigV1 config,
    CssStyleMaterializationBatchV1* output,
    CssStyleMaterializationStatsV1* stats,
    CssStyleMaterializationErrorV1* error) noexcept {
    if (stats != nullptr) {
        *stats = CssStyleMaterializationStatsV1{};
    }
    if (error != nullptr) {
        error->kind = CssStyleMaterializationErrorKindV1::None;
        error->request_index = 0U;
        error->message.clear();
    }
    if (output == nullptr || stats == nullptr || error == nullptr ||
        output->resource() == nullptr || !config.valid()) {
        return set_error(
            error,
            CssStyleMaterializationErrorKindV1::InvalidConfiguration,
            0U,
            "CSS style materialization output, stats, error and valid configuration are required");
    }
    if (terminal_nodes.size() > config.maximum_requests) {
        return set_error(
            error,
            CssStyleMaterializationErrorKindV1::RequestLimitExceeded,
            config.maximum_requests,
            "CSS style materialization request count exceeds configured limit");
    }

    CssStyleMaterializationBatchV1 candidate(output->resource());
    CssStyleMaterializationStatsV1 candidate_stats;
    candidate_stats.requests =
        static_cast<std::uint64_t>(terminal_nodes.size());

    try {
        if (!terminal_nodes.empty() &&
            !validate_root(dag, config, &candidate_stats, error)) {
            *stats = candidate_stats;
            return false;
        }

        for (std::size_t request_index = 0U;
             request_index < terminal_nodes.size();
             ++request_index) {
            const std::uint32_t terminal = terminal_nodes[request_index];
            if (static_cast<std::size_t>(terminal) >= dag.nodes.size()) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssStyleMaterializationErrorKindV1::InvalidTerminalNode,
                    request_index,
                    "CSS style materialization terminal node is out of range");
            }

            std::uint32_t existing_style =
                kCssStyleDagNoNodeV1;
            for (std::size_t style_index = 0U;
                 style_index < candidate.styles.size();
                 ++style_index) {
                if (!consume_work(config, &candidate_stats, 1U)) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssStyleMaterializationErrorKindV1::WorkBudgetExceeded,
                        request_index,
                        "CSS style materialization duplicate lookup exceeded work budget");
                }
                ++candidate_stats.duplicate_lookup_comparisons;
                if (candidate.styles[style_index].terminal_node == terminal) {
                    if (style_index >
                        static_cast<std::size_t>(
                            std::numeric_limits<std::uint32_t>::max())) {
                        *stats = candidate_stats;
                        return set_error(
                            error,
                            CssStyleMaterializationErrorKindV1::RepresentationOverflow,
                            request_index,
                            "CSS style materialization style index exceeds 32-bit mapping");
                    }
                    existing_style =
                        static_cast<std::uint32_t>(style_index);
                    break;
                }
            }
            if (existing_style != kCssStyleDagNoNodeV1) {
                candidate.request_style_indices.push_back(existing_style);
                ++candidate_stats.duplicate_style_requests;
                continue;
            }

            if (candidate.styles.size() >= config.maximum_unique_styles) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssStyleMaterializationErrorKindV1::UniqueStyleLimitExceeded,
                    request_index,
                    "CSS style materialization unique style count exceeds configured limit");
            }

            std::pmr::vector<std::uint32_t> chain(output->resource());
            std::uint32_t cursor = terminal;
            while (cursor != 0U) {
                if (chain.size() >= config.maximum_properties_per_style) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssStyleMaterializationErrorKindV1::PropertiesPerStyleExceeded,
                        request_index,
                        "CSS style materialization style depth exceeds configured property limit");
                }
                if (!validate_chain_node(
                        dag,
                        cursor,
                        config,
                        &candidate_stats,
                        request_index,
                        error)) {
                    *stats = candidate_stats;
                    return false;
                }
                chain.push_back(cursor);
                cursor =
                    dag.nodes[static_cast<std::size_t>(cursor)].parent;
            }

            if (candidate.properties.size() >
                    static_cast<std::size_t>(
                        std::numeric_limits<std::uint32_t>::max()) ||
                chain.size() >
                    static_cast<std::size_t>(
                        std::numeric_limits<std::uint32_t>::max())) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssStyleMaterializationErrorKindV1::RepresentationOverflow,
                    request_index,
                    "CSS style materialization property range exceeds 32-bit representation");
            }
            if (chain.size() >
                config.maximum_properties - candidate.properties.size()) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssStyleMaterializationErrorKindV1::PropertyLimitExceeded,
                    request_index,
                    "CSS style materialization total property count exceeds configured limit");
            }

            CssMaterializedStyleV1 style;
            style.terminal_node = terminal;
            style.property_offset =
                static_cast<std::uint32_t>(candidate.properties.size());
            style.property_count =
                static_cast<std::uint32_t>(chain.size());

            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const CssStyleDagNodeV1& node =
                    dag.nodes[static_cast<std::size_t>(*it)];
                const std::string_view property =
                    dag.resolve(node.property);
                const std::string_view value =
                    dag.resolve(node.value);
                const std::uint64_t copy_units =
                    static_cast<std::uint64_t>(property.size()) +
                    static_cast<std::uint64_t>(value.size());
                if (!consume_work(
                        config,
                        &candidate_stats,
                        copy_units)) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssStyleMaterializationErrorKindV1::WorkBudgetExceeded,
                        request_index,
                        "CSS style materialization text copy exceeded work budget");
                }

                CssMaterializedPropertyV1 materialized;
                if (!append_text(
                        property,
                        config,
                        &candidate,
                        &materialized.property,
                        &candidate_stats) ||
                    !append_text(
                        value,
                        config,
                        &candidate,
                        &materialized.value,
                        &candidate_stats)) {
                    *stats = candidate_stats;
                    const bool representation =
                        property.size() >
                            static_cast<std::size_t>(
                                std::numeric_limits<std::uint32_t>::max()) ||
                        value.size() >
                            static_cast<std::size_t>(
                                std::numeric_limits<std::uint32_t>::max());
                    return set_error(
                        error,
                        representation
                            ? CssStyleMaterializationErrorKindV1::RepresentationOverflow
                            : CssStyleMaterializationErrorKindV1::TextBudgetExceeded,
                        request_index,
                        "CSS style materialization text cannot fit output budget");
                }
                candidate.properties.push_back(materialized);
                ++candidate_stats.properties_materialized;
            }

            if (candidate.styles.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssStyleMaterializationErrorKindV1::RepresentationOverflow,
                    request_index,
                    "CSS style materialization style count exceeds 32-bit request mapping");
            }
            const std::uint32_t style_index =
                static_cast<std::uint32_t>(candidate.styles.size());
            candidate.styles.push_back(style);
            candidate.request_style_indices.push_back(style_index);
            ++candidate_stats.unique_styles;
        }

        output->release();
        output->text.swap(candidate.text);
        output->properties.swap(candidate.properties);
        output->styles.swap(candidate.styles);
        output->request_style_indices.swap(
            candidate.request_style_indices);
        *stats = candidate_stats;
        return true;
    } catch (const std::bad_alloc&) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssStyleMaterializationErrorKindV1::AllocationFailure,
            0U,
            "CSS style materialization allocation rejected by bounded memory resource");
    } catch (...) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssStyleMaterializationErrorKindV1::AllocationFailure,
            0U,
            "CSS style materialization allocation or container operation failed");
    }
}

} // namespace zevryon::style
