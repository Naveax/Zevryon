// Keep the existing tab-runtime implementation byte-for-byte intact while
// extending the same translation unit with the source-record semantic bridge.
// CMake compiles this wrapper instead of compiling zenith_tab_runtime.cpp
// separately, so there is still exactly one definition of every runtime symbol.
#include "zenith_tab_runtime.cpp"

#include "logical_node_record_index.hpp"
#include "zenith_semantic_runtime_consumer.hpp"

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

    if (!impl_->opened) {
        impl_->update_statistics([](ZenithTabRuntimeStats& stats) {
            stats.semantic_record_failures = semantic_saturating_add(
                stats.semantic_record_failures,
                1U);
        });
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
        impl_->update_statistics([](ZenithTabRuntimeStats& stats) {
            stats.semantic_record_failures = semantic_saturating_add(
                stats.semantic_record_failures,
                1U);
        });
        *error = "invalid zenith source-record semantic query bounds";
        return false;
    }

    LogicalNodeRecordIndexAuthoritativeReader index(impl_->root);
    if (!index.open(error)) {
        impl_->update_statistics([](ZenithTabRuntimeStats& stats) {
            stats.semantic_record_failures = semantic_saturating_add(
                stats.semantic_record_failures,
                1U);
        });
        return false;
    }

    LogicalNodeRecordIndexWindow posting_window;
    if (!index.read_record(
            source_record_index,
            continuation_posting_ordinal,
            max_nodes,
            &posting_window,
            error)) {
        impl_->update_statistics([](ZenithTabRuntimeStats& stats) {
            stats.semantic_record_failures = semantic_saturating_add(
                stats.semantic_record_failures,
                1U);
        });
        return false;
    }

    ZenithRecordSemanticWindowResult decoded;
    decoded.source_record_index = source_record_index;
    decoded.next_posting_ordinal = posting_window.next_posting_ordinal;
    decoded.truncated = posting_window.truncated;
    decoded.nodes.reserve(posting_window.postings.size());

    if (!posting_window.postings.empty()) {
        ZenithSemanticNodeWindowConfig semantic_config =
            impl_->config.semantic_window;
        semantic_config.maximum_nodes = 1U;
        ZenithSemanticRuntimeConsumer semantics(impl_->root, semantic_config);

        for (const LogicalNodeRecordPosting& posting : posting_window.postings) {
            ZenithSemanticNodeWindowResult node_window;
            if (!semantics.read_on_lane(
                    FrameExecutionLane::Worker,
                    posting.node_ordinal,
                    &node_window,
                    error) ||
                node_window.start_ordinal != posting.node_ordinal ||
                node_window.nodes.size() != 1U) {
                if (error->empty()) {
                    *error =
                        "zenith source-record semantic materialization did not return the indexed node";
                }
                impl_->update_statistics([](ZenithTabRuntimeStats& stats) {
                    stats.semantic_record_failures = semantic_saturating_add(
                        stats.semantic_record_failures,
                        1U);
                });
                return false;
            }

            ZenithRecordSemanticNode resolved;
            resolved.source_overlap = posting;
            resolved.semantic = std::move(node_window.nodes.front());
            decoded.nodes.push_back(std::move(resolved));
        }
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
