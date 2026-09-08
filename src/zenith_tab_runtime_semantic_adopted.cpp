// Keep the existing tab-runtime implementation byte-for-byte intact while
// extending the same translation unit with the source-record semantic bridge.
// CMake compiles this wrapper instead of compiling zenith_tab_runtime.cpp
// separately, so there is still exactly one definition of every runtime symbol.
#include "zenith_tab_runtime.cpp"

#include "logical_node_record_index.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace zevryon::massivedoc {
namespace {

std::uint64_t semantic_saturating_add(
    std::uint64_t left,
    std::uint64_t right) noexcept {
    return left > std::numeric_limits<std::uint64_t>::max() - right
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

bool add_semantic_size(std::size_t* total, std::size_t amount) noexcept {
    if (total == nullptr ||
        *total > std::numeric_limits<std::size_t>::max() - amount) {
        return false;
    }
    *total += amount;
    return true;
}

bool materialize_same_arena_node(
    const LogicalNodeRecordIndexAuthoritativeReader& index,
    std::uint64_t node_ordinal,
    const ZenithSemanticNodeWindowConfig& config,
    ZenithSemanticNode* node,
    std::size_t* attribute_count,
    std::size_t* semantic_bytes,
    std::string* error) {
    if (node == nullptr || attribute_count == nullptr ||
        semantic_bytes == nullptr || error == nullptr) {
        return false;
    }
    *node = ZenithSemanticNode{};
    *attribute_count = 0U;
    *semantic_bytes = 0U;

    LogicalNodeRecord record;
    if (!index.node_by_ordinal(node_ordinal, &record, error)) {
        return false;
    }
    if (record.attribute_count > config.maximum_attributes_per_node) {
        *error =
            "zenith source-record semantic node exceeds per-node attribute budget";
        return false;
    }

    node->record = record;
    if (!index.resolve_semantic(
            LogicalSemanticKind::tag, record.tag_id, &node->tag, error) ||
        !index.resolve_semantic(
            LogicalSemanticKind::role, record.role_id, &node->role, error) ||
        !index.resolve_semantic(
            LogicalSemanticKind::style, record.style_id, &node->style, error)) {
        return false;
    }
    if (!add_semantic_size(semantic_bytes, node->tag.size()) ||
        !add_semantic_size(semantic_bytes, node->role.size()) ||
        !add_semantic_size(semantic_bytes, node->style.size())) {
        *error = "zenith source-record semantic byte accounting overflows";
        return false;
    }

    const std::size_t count = static_cast<std::size_t>(record.attribute_count);
    node->attributes.reserve(count);
    for (std::uint32_t relative = 0U;
         relative < record.attribute_count;
         ++relative) {
        if (record.attribute_offset >
            std::numeric_limits<std::uint64_t>::max() - relative) {
            *error = "zenith source-record semantic attribute ordinal overflows";
            return false;
        }
        LogicalNodeAttributeRecord stored;
        if (!index.attribute_by_ordinal(
                record.attribute_offset + relative,
                &stored,
                error)) {
            return false;
        }

        ZenithSemanticAttribute attribute;
        attribute.flags = stored.flags;
        if (!index.resolve_semantic(
                LogicalSemanticKind::attribute_name,
                stored.name_id,
                &attribute.name,
                error) ||
            !index.resolve_semantic(
                LogicalSemanticKind::attribute_value,
                stored.value_id,
                &attribute.value,
                error)) {
            return false;
        }
        if (!add_semantic_size(semantic_bytes, attribute.name.size()) ||
            !add_semantic_size(semantic_bytes, attribute.value.size())) {
            *error = "zenith source-record semantic byte accounting overflows";
            return false;
        }
        node->attributes.push_back(std::move(attribute));
    }

    *attribute_count = count;
    return true;
}

} // namespace

bool ZenithTabRuntime::semantic_nodes_for_source_record(
    std::uint64_t source_record_index,
    std::uint64_t continuation_posting_ordinal,
    std::size_t max_nodes,
    ZenithRecordSemanticWindowResult* result,
    std::string* error) {
    return semantic_nodes_for_source_record_on_lane(
        FrameExecutionLane::Worker,
        source_record_index,
        continuation_posting_ordinal,
        max_nodes,
        result,
        error);
}

bool ZenithTabRuntime::semantic_nodes_for_source_record_on_lane(
    FrameExecutionLane lane,
    std::uint64_t source_record_index,
    std::uint64_t continuation_posting_ordinal,
    std::size_t max_nodes,
    ZenithRecordSemanticWindowResult* result,
    std::string* error) {
    if (result == nullptr || error == nullptr ||
        (lane != FrameExecutionLane::Ui && lane != FrameExecutionLane::Worker)) {
        if (error != nullptr) {
            *error = "invalid zenith source-record semantic request";
        }
        return false;
    }
    *result = ZenithRecordSemanticWindowResult{};
    error->clear();

    impl_->update_statistics([](ZenithTabRuntimeStats& stats) {
        stats.semantic_record_requests = semantic_saturating_add(
            stats.semantic_record_requests,
            1U);
    });
    const auto mark_failure = [this]() {
        impl_->update_statistics([](ZenithTabRuntimeStats& stats) {
            stats.semantic_record_failures = semantic_saturating_add(
                stats.semantic_record_failures,
                1U);
        });
    };

    if (!impl_->opened) {
        mark_failure();
        *error = "zenith tab runtime is not open for semantic queries";
        return false;
    }

    // The lane fence intentionally precedes all record-index/arena objects.
    // A UI-lane rejection therefore cannot open semantic sidecar files.
    if (lane == FrameExecutionLane::Ui) {
        impl_->update_statistics([](ZenithTabRuntimeStats& stats) {
            stats.ui_semantic_record_rejections = semantic_saturating_add(
                stats.ui_semantic_record_rejections,
                1U);
        });
        *error =
            "blocking source-record semantic I/O is forbidden on the UI execution lane";
        return false;
    }

    if (max_nodes == 0U ||
        max_nodes > kMaximumLogicalNodeRecordWindowNodes ||
        !impl_->config.semantic_window.valid()) {
        mark_failure();
        *error = "invalid zenith source-record semantic query bounds";
        return false;
    }

    LogicalNodeRecordIndexAuthoritativeReader index(impl_->root);
    if (!index.open(error)) {
        mark_failure();
        return false;
    }

    const std::size_t effective_max_nodes = std::min(
        max_nodes,
        impl_->config.semantic_window.maximum_nodes);
    LogicalNodeRecordIndexWindow posting_window;
    if (!index.read_record(
            source_record_index,
            continuation_posting_ordinal,
            effective_max_nodes,
            &posting_window,
            error)) {
        mark_failure();
        return false;
    }

    ZenithRecordSemanticWindowResult decoded;
    decoded.source_record_index = source_record_index;
    decoded.next_posting_ordinal = posting_window.next_posting_ordinal;
    decoded.truncated = posting_window.truncated;
    decoded.nodes.reserve(posting_window.postings.size());

    for (const LogicalNodeRecordPosting& posting : posting_window.postings) {
        ZenithSemanticNode semantic;
        std::size_t candidate_attributes = 0U;
        std::size_t candidate_bytes = 0U;
        if (!materialize_same_arena_node(
                index,
                posting.node_ordinal,
                impl_->config.semantic_window,
                &semantic,
                &candidate_attributes,
                &candidate_bytes,
                error)) {
            mark_failure();
            return false;
        }

        if (candidate_attributes >
            impl_->config.semantic_window.maximum_total_attributes) {
            mark_failure();
            *error =
                "zenith source-record semantic node exceeds total attribute budget by itself";
            return false;
        }
        if (decoded.attribute_count >
            impl_->config.semantic_window.maximum_total_attributes -
                candidate_attributes) {
            decoded.truncated = true;
            decoded.next_posting_ordinal = posting.posting_ordinal;
            break;
        }

        if (candidate_bytes >
            impl_->config.semantic_window.maximum_semantic_bytes) {
            if (decoded.nodes.empty()) {
                mark_failure();
                *error =
                    "zenith source-record semantic node exceeds semantic-byte budget by itself";
                return false;
            }
            decoded.truncated = true;
            decoded.next_posting_ordinal = posting.posting_ordinal;
            break;
        }
        if (decoded.semantic_bytes >
            impl_->config.semantic_window.maximum_semantic_bytes -
                candidate_bytes) {
            decoded.truncated = true;
            decoded.next_posting_ordinal = posting.posting_ordinal;
            break;
        }

        ZenithRecordSemanticNode resolved;
        resolved.source_overlap = posting;
        resolved.semantic = std::move(semantic);
        decoded.attribute_count += candidate_attributes;
        decoded.semantic_bytes += candidate_bytes;
        decoded.nodes.push_back(std::move(resolved));
    }

    impl_->update_statistics([&](ZenithTabRuntimeStats& stats) {
        stats.semantic_record_successes = semantic_saturating_add(
            stats.semantic_record_successes,
            1U);
        stats.semantic_nodes_materialized = semantic_saturating_add(
            stats.semantic_nodes_materialized,
            static_cast<std::uint64_t>(decoded.nodes.size()));
    });
    *result = std::move(decoded);
    return true;
}

} // namespace zevryon::massivedoc
