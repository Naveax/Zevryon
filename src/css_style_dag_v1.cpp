#include "css_style_dag_v1.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace zevryon::style {
namespace {

bool set_error(
    CssStyleDagErrorV1* error,
    CssStyleDagErrorKindV1 kind,
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

bool stylesheet_slice_valid(
    const CssStylesheetV1& stylesheet,
    CssTextSliceV1 slice) noexcept {
    const std::size_t offset = static_cast<std::size_t>(slice.offset);
    const std::size_t length = static_cast<std::size_t>(slice.length);
    return offset <= stylesheet.text.size() &&
        length <= stylesheet.text.size() - offset;
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
    CssStyleDagConfigV1 config,
    CssStyleDagStatsV1* stats,
    std::uint64_t units) noexcept {
    if (stats == nullptr ||
        stats->work_units > config.maximum_work_units ||
        units > config.maximum_work_units - stats->work_units) {
        return false;
    }
    stats->work_units += units;
    return true;
}

bool compare_text(
    std::string_view left,
    std::string_view right,
    CssStyleDagConfigV1 config,
    CssStyleDagStatsV1* stats,
    std::uint64_t* comparison_counter,
    int* result) noexcept {
    if (result == nullptr || comparison_counter == nullptr) {
        return false;
    }
    const std::uint64_t units =
        static_cast<std::uint64_t>(left.size()) +
        static_cast<std::uint64_t>(right.size()) +
        1U;
    if (!consume_work(config, stats, units)) {
        return false;
    }
    ++(*comparison_counter);
    const int cmp = left.compare(right);
    *result = cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
    return true;
}

bool append_text(
    std::string_view value,
    CssStyleDagConfigV1 config,
    CssComputedStyleDagV1* dag,
    CssTextSliceV1* slice,
    CssStyleDagStatsV1* stats) {
    if (dag == nullptr || slice == nullptr || stats == nullptr) {
        return false;
    }
    if (dag->text.size() > config.maximum_text_bytes ||
        value.size() > config.maximum_text_bytes - dag->text.size()) {
        return false;
    }
    if (dag->text.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()) ||
        value.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }

    slice->offset =
        static_cast<std::uint32_t>(dag->text.size());
    slice->length = static_cast<std::uint32_t>(value.size());
    dag->text.append(value.data(), value.size());
    stats->text_bytes_appended +=
        static_cast<std::uint64_t>(value.size());
    return true;
}

bool validate_existing_dag(
    const CssComputedStyleDagV1& dag,
    CssStyleDagConfigV1 config,
    CssStyleDagStatsV1* stats,
    CssStyleDagErrorV1* error) noexcept {
    if (dag.nodes.size() > config.maximum_nodes ||
        dag.text.size() > config.maximum_text_bytes) {
        return set_error(
            error,
            CssStyleDagErrorKindV1::CorruptDag,
            0U,
            "CSS style DAG exceeds configured retained bounds");
    }
    if (dag.nodes.empty()) {
        return true;
    }
    if (!consume_work(config, stats, 1U)) {
        return set_error(
            error,
            CssStyleDagErrorKindV1::WorkBudgetExceeded,
            0U,
            "CSS style DAG root validation exceeded work budget");
    }

    const CssStyleDagNodeV1& root = dag.nodes.front();
    if (root.parent != kCssStyleDagNoNodeV1 ||
        root.property.length != 0U ||
        root.value.length != 0U ||
        root.depth != 0U) {
        return set_error(
            error,
            CssStyleDagErrorKindV1::CorruptDag,
            0U,
            "CSS style DAG root node is invalid");
    }

    for (std::size_t index = 1U; index < dag.nodes.size(); ++index) {
        if (!consume_work(config, stats, 1U)) {
            return set_error(
                error,
                CssStyleDagErrorKindV1::WorkBudgetExceeded,
                index,
                "CSS style DAG retained-node validation exceeded work budget");
        }
        const CssStyleDagNodeV1& node = dag.nodes[index];
        if (node.parent >= index ||
            !dag_slice_valid(dag, node.property) ||
            !dag_slice_valid(dag, node.value) ||
            node.property.length == 0U) {
            return set_error(
                error,
                CssStyleDagErrorKindV1::CorruptDag,
                index,
                "CSS style DAG node topology or text slice is invalid");
        }
        const CssStyleDagNodeV1& parent =
            dag.nodes[static_cast<std::size_t>(node.parent)];
        if (node.depth != parent.depth + 1U) {
            return set_error(
                error,
                CssStyleDagErrorKindV1::CorruptDag,
                index,
                "CSS style DAG node depth is inconsistent with parent");
        }
        if (node.parent != 0U) {
            int comparison = 0;
            if (!compare_text(
                    dag.resolve(parent.property),
                    dag.resolve(node.property),
                    config,
                    stats,
                    &stats->retained_validation_comparisons,
                    &comparison)) {
                return set_error(
                    error,
                    CssStyleDagErrorKindV1::WorkBudgetExceeded,
                    index,
                    "CSS style DAG canonical retained-order validation exceeded work budget");
            }
            if (comparison >= 0) {
                return set_error(
                    error,
                    CssStyleDagErrorKindV1::CorruptDag,
                    index,
                    "CSS style DAG retained property order is not strictly canonical");
            }
        }
    }
    return true;
}

bool ensure_root(
    CssComputedStyleDagV1* dag,
    CssStyleDagConfigV1 config,
    CssStyleDagStatsV1* stats,
    CssStyleDagErrorV1* error) {
    if (dag == nullptr || stats == nullptr) {
        return false;
    }
    if (!dag->nodes.empty()) {
        return true;
    }
    if (config.maximum_nodes == 0U) {
        return set_error(
            error,
            CssStyleDagErrorKindV1::NodeLimitExceeded,
            0U,
            "CSS style DAG cannot allocate root inside node limit");
    }
    dag->nodes.push_back(CssStyleDagNodeV1{});
    ++stats->nodes_created;
    return true;
}

} // namespace

CssComputedStyleDagV1::CssComputedStyleDagV1(
    std::pmr::memory_resource* memory)
    : text(memory), nodes(memory) {}

std::pmr::memory_resource*
CssComputedStyleDagV1::resource() const noexcept {
    return text.get_allocator().resource();
}

std::string_view CssComputedStyleDagV1::resolve(
    CssTextSliceV1 slice) const noexcept {
    const std::size_t offset = static_cast<std::size_t>(slice.offset);
    const std::size_t length = static_cast<std::size_t>(slice.length);
    if (offset > text.size() || length > text.size() - offset) {
        return {};
    }
    return std::string_view(text.data() + offset, length);
}

void CssComputedStyleDagV1::release() noexcept {
    std::pmr::string empty_text(resource());
    std::pmr::vector<CssStyleDagNodeV1> empty_nodes(resource());
    text.swap(empty_text);
    nodes.swap(empty_nodes);
}

bool CssStyleDagConfigV1::valid() const noexcept {
    return maximum_properties_per_style > 0U &&
        maximum_properties_per_style <= kMaximumPropertiesPerStyleLimit &&
        maximum_nodes > 0U &&
        maximum_nodes <= kMaximumNodesLimit &&
        maximum_text_bytes > 0U &&
        maximum_text_bytes <= kMaximumTextBytesLimit &&
        maximum_text_bytes <=
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()) &&
        maximum_style_semantic_bytes > 0U &&
        maximum_style_semantic_bytes <=
            kMaximumStyleSemanticBytesLimit &&
        maximum_work_units > 0U &&
        maximum_work_units <= kMaximumWorkUnitsLimit;
}

const char* css_style_dag_error_kind_name_v1(
    CssStyleDagErrorKindV1 kind) noexcept {
    switch (kind) {
    case CssStyleDagErrorKindV1::None:
        return "none";
    case CssStyleDagErrorKindV1::InvalidConfiguration:
        return "invalid-configuration";
    case CssStyleDagErrorKindV1::PropertyLimitExceeded:
        return "property-limit-exceeded";
    case CssStyleDagErrorKindV1::NodeLimitExceeded:
        return "node-limit-exceeded";
    case CssStyleDagErrorKindV1::TextBudgetExceeded:
        return "text-budget-exceeded";
    case CssStyleDagErrorKindV1::SemanticBudgetExceeded:
        return "semantic-budget-exceeded";
    case CssStyleDagErrorKindV1::WorkBudgetExceeded:
        return "work-budget-exceeded";
    case CssStyleDagErrorKindV1::InvalidStylesheet:
        return "invalid-stylesheet";
    case CssStyleDagErrorKindV1::InvalidCascadeResult:
        return "invalid-cascade-result";
    case CssStyleDagErrorKindV1::CorruptDag:
        return "corrupt-dag";
    case CssStyleDagErrorKindV1::RepresentationOverflow:
        return "representation-overflow";
    case CssStyleDagErrorKindV1::AllocationFailure:
        return "allocation-failure";
    }
    return "unknown";
}

bool intern_css_cascade_style_v1(
    const CssStylesheetV1& stylesheet,
    const CssCascadeResultV1& cascade,
    CssStyleDagConfigV1 config,
    CssComputedStyleDagV1* dag,
    std::uint32_t* terminal_node,
    CssStyleDagStatsV1* stats,
    CssStyleDagErrorV1* error) noexcept {
    if (stats != nullptr) {
        *stats = CssStyleDagStatsV1{};
    }
    if (terminal_node != nullptr) {
        *terminal_node = kCssStyleDagNoNodeV1;
    }
    if (error != nullptr) {
        error->kind = CssStyleDagErrorKindV1::None;
        error->index = 0U;
        error->message.clear();
    }
    if (dag == nullptr || terminal_node == nullptr ||
        stats == nullptr || error == nullptr ||
        dag->resource() == nullptr || !config.valid()) {
        return set_error(
            error,
            CssStyleDagErrorKindV1::InvalidConfiguration,
            0U,
            "CSS style DAG, terminal output, stats, error and valid configuration are required");
    }
    if (cascade.winners.size() > config.maximum_properties_per_style) {
        return set_error(
            error,
            CssStyleDagErrorKindV1::PropertyLimitExceeded,
            config.maximum_properties_per_style,
            "CSS computed style exceeds configured property count");
    }
    CssStyleDagStatsV1 candidate_stats;
    if (!validate_existing_dag(
            *dag,
            config,
            &candidate_stats,
            error)) {
        *stats = candidate_stats;
        return false;
    }

    const std::size_t old_node_size = dag->nodes.size();
    const std::size_t old_text_size = dag->text.size();

    const auto rollback = [&]() noexcept {
        try {
            dag->nodes.resize(old_node_size);
            dag->text.resize(old_text_size);
        } catch (...) {
        }
    };

    try {
        if (!ensure_root(dag, config, &candidate_stats, error)) {
            rollback();
            *stats = candidate_stats;
            return false;
        }

        std::pmr::vector<std::size_t> order(dag->resource());
        order.reserve(cascade.winners.size());

        std::size_t semantic_bytes = 0U;
        for (std::size_t winner_index = 0U;
             winner_index < cascade.winners.size();
             ++winner_index) {
            const CssCascadeWinnerV1& winner =
                cascade.winners[winner_index];
            ++candidate_stats.properties_considered;
            if (!stylesheet_slice_valid(stylesheet, winner.property) ||
                !stylesheet_slice_valid(stylesheet, winner.value) ||
                winner.property.length == 0U) {
                rollback();
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssStyleDagErrorKindV1::InvalidCascadeResult,
                    winner_index,
                    "CSS style DAG encountered invalid cascade winner slices");
            }

            const std::size_t property_bytes =
                static_cast<std::size_t>(winner.property.length);
            const std::size_t value_bytes =
                static_cast<std::size_t>(winner.value.length);
            if (semantic_bytes >
                    config.maximum_style_semantic_bytes ||
                property_bytes >
                    config.maximum_style_semantic_bytes -
                        semantic_bytes) {
                rollback();
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssStyleDagErrorKindV1::SemanticBudgetExceeded,
                    winner_index,
                    "CSS style DAG property bytes exceed style semantic budget");
            }
            semantic_bytes += property_bytes;
            if (value_bytes >
                    config.maximum_style_semantic_bytes -
                        semantic_bytes) {
                rollback();
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssStyleDagErrorKindV1::SemanticBudgetExceeded,
                    winner_index,
                    "CSS style DAG value bytes exceed style semantic budget");
            }
            semantic_bytes += value_bytes;

            const std::string_view property =
                stylesheet.resolve(winner.property);
            std::size_t insertion = order.size();
            while (insertion > 0U) {
                const std::size_t previous_index = order[insertion - 1U];
                const std::string_view previous =
                    stylesheet.resolve(
                        cascade.winners[previous_index].property);
                int comparison = 0;
                if (!compare_text(
                        property,
                        previous,
                        config,
                        &candidate_stats,
                        &candidate_stats.canonical_order_comparisons,
                        &comparison)) {
                    rollback();
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssStyleDagErrorKindV1::WorkBudgetExceeded,
                        winner_index,
                        "CSS style DAG canonical ordering exceeded work budget");
                }
                if (comparison == 0) {
                    rollback();
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssStyleDagErrorKindV1::InvalidCascadeResult,
                        winner_index,
                        "CSS cascade result contains duplicate winning property");
                }
                if (comparison > 0) {
                    break;
                }
                --insertion;
            }
            order.insert(
                order.begin() +
                    static_cast<std::ptrdiff_t>(insertion),
                winner_index);
        }

        std::uint32_t parent = 0U;
        for (const std::size_t winner_index : order) {
            const CssCascadeWinnerV1& winner =
                cascade.winners[winner_index];
            const std::string_view property =
                stylesheet.resolve(winner.property);
            const std::string_view value =
                stylesheet.resolve(winner.value);

            std::uint32_t child = kCssStyleDagNoNodeV1;
            for (std::size_t node_index = 1U;
                 node_index < dag->nodes.size();
                 ++node_index) {
                if (!consume_work(
                        config,
                        &candidate_stats,
                        1U)) {
                    rollback();
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssStyleDagErrorKindV1::WorkBudgetExceeded,
                        winner_index,
                        "CSS style DAG child scan exceeded work budget");
                }
                ++candidate_stats.child_lookup_nodes_scanned;
                const CssStyleDagNodeV1& node =
                    dag->nodes[node_index];
                if (node.parent != parent) {
                    continue;
                }
                if (!dag_slice_valid(*dag, node.property) ||
                    !dag_slice_valid(*dag, node.value)) {
                    rollback();
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssStyleDagErrorKindV1::CorruptDag,
                        node_index,
                        "CSS style DAG child lookup encountered corrupt text slice");
                }

                int property_comparison = 0;
                if (!compare_text(
                        property,
                        dag->resolve(node.property),
                        config,
                        &candidate_stats,
                        &candidate_stats.child_lookup_comparisons,
                        &property_comparison)) {
                    rollback();
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssStyleDagErrorKindV1::WorkBudgetExceeded,
                        winner_index,
                        "CSS style DAG child property lookup exceeded work budget");
                }
                if (property_comparison != 0) {
                    continue;
                }

                int value_comparison = 0;
                if (!compare_text(
                        value,
                        dag->resolve(node.value),
                        config,
                        &candidate_stats,
                        &candidate_stats.child_lookup_comparisons,
                        &value_comparison)) {
                    rollback();
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssStyleDagErrorKindV1::WorkBudgetExceeded,
                        winner_index,
                        "CSS style DAG child value lookup exceeded work budget");
                }
                if (value_comparison == 0) {
                    if (node_index >
                        static_cast<std::size_t>(
                            std::numeric_limits<std::uint32_t>::max())) {
                        rollback();
                        *stats = candidate_stats;
                        return set_error(
                            error,
                            CssStyleDagErrorKindV1::RepresentationOverflow,
                            node_index,
                            "CSS style DAG node index exceeds 32-bit identity");
                    }
                    child = static_cast<std::uint32_t>(node_index);
                    ++candidate_stats.nodes_reused;
                    break;
                }
            }

            if (child == kCssStyleDagNoNodeV1) {
                if (dag->nodes.size() >= config.maximum_nodes) {
                    rollback();
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssStyleDagErrorKindV1::NodeLimitExceeded,
                        winner_index,
                        "CSS style DAG node count exceeds configured limit");
                }
                if (dag->nodes.size() >
                    static_cast<std::size_t>(
                        std::numeric_limits<std::uint32_t>::max())) {
                    rollback();
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssStyleDagErrorKindV1::RepresentationOverflow,
                        winner_index,
                        "CSS style DAG node count exceeds 32-bit identity");
                }

                CssStyleDagNodeV1 node;
                node.parent = parent;
                const CssStyleDagNodeV1& parent_node =
                    dag->nodes[static_cast<std::size_t>(parent)];
                if (parent_node.depth ==
                    std::numeric_limits<std::uint32_t>::max()) {
                    rollback();
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssStyleDagErrorKindV1::RepresentationOverflow,
                        winner_index,
                        "CSS style DAG depth exceeds 32-bit representation");
                }
                node.depth = parent_node.depth + 1U;

                const std::size_t text_before = dag->text.size();
                if (!append_text(
                        property,
                        config,
                        dag,
                        &node.property,
                        &candidate_stats)) {
                    rollback();
                    *stats = candidate_stats;
                    const bool representation =
                        dag->text.size() >
                            static_cast<std::size_t>(
                                std::numeric_limits<std::uint32_t>::max()) ||
                        property.size() >
                            static_cast<std::size_t>(
                                std::numeric_limits<std::uint32_t>::max());
                    return set_error(
                        error,
                        representation
                            ? CssStyleDagErrorKindV1::RepresentationOverflow
                            : CssStyleDagErrorKindV1::TextBudgetExceeded,
                        winner_index,
                        "CSS style DAG property cannot fit retained text budget");
                }
                if (!append_text(
                        value,
                        config,
                        dag,
                        &node.value,
                        &candidate_stats)) {
                    dag->text.resize(text_before);
                    rollback();
                    *stats = candidate_stats;
                    const bool representation =
                        value.size() >
                            static_cast<std::size_t>(
                                std::numeric_limits<std::uint32_t>::max());
                    return set_error(
                        error,
                        representation
                            ? CssStyleDagErrorKindV1::RepresentationOverflow
                            : CssStyleDagErrorKindV1::TextBudgetExceeded,
                        winner_index,
                        "CSS style DAG value cannot fit retained text budget");
                }

                dag->nodes.push_back(node);
                child = static_cast<std::uint32_t>(
                    dag->nodes.size() - 1U);
                ++candidate_stats.nodes_created;
            }
            parent = child;
        }

        *terminal_node = parent;
        *stats = candidate_stats;
        return true;
    } catch (const std::bad_alloc&) {
        rollback();
        *stats = candidate_stats;
        return set_error(
            error,
            CssStyleDagErrorKindV1::AllocationFailure,
            0U,
            "CSS style DAG allocation rejected by bounded memory resource");
    } catch (...) {
        rollback();
        *stats = candidate_stats;
        return set_error(
            error,
            CssStyleDagErrorKindV1::AllocationFailure,
            0U,
            "CSS style DAG allocation or container operation failed");
    }
}

} // namespace zevryon::style
