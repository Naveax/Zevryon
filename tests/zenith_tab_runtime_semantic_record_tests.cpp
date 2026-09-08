#include "compact_document.hpp"
#include "logical_node_arena.hpp"
#include "logical_node_arena_v2_store_bound.hpp"
#include "logical_node_record_index.hpp"
#include "logical_node_source.hpp"
#include "massivedoc_store.hpp"
#include "zenith_tab_runtime.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace zevryon::massivedoc;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: zenith tab semantic record: " << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path unique_root(std::string_view name) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("zevryon-") + std::string(name) + "-" +
         std::to_string(tick));
}

struct RootCleanup {
    explicit RootCleanup(std::filesystem::path value) : root(std::move(value)) {}
    ~RootCleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    RootCleanup(const RootCleanup&) = delete;
    RootCleanup& operator=(const RootCleanup&) = delete;
    std::filesystem::path root;
};

std::span<const std::byte> bytes(std::string_view text) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()),
        text.size());
}

bool build_store_and_layout_arena(
    const std::filesystem::path& root,
    std::string* error) {
    constexpr std::array<std::string_view, 3> records{{"ab", "cdef", "gh"}};
    StoreWriter writer(root);
    for (std::size_t index = 0U; index < records.size(); ++index) {
        if (!writer.append(
                9901U + static_cast<std::uint64_t>(index),
                bytes(records[index]),
                error)) {
            return false;
        }
    }

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = 8U;
    metadata.logical_records = 3U;
    metadata.logical_nodes = 4U;
    metadata.largest_record_bytes = 4U;
    if (!writer.finalize(metadata, nullptr, error)) {
        return false;
    }

    ArenaConfig arena_config;
    arena_config.records_per_block = 1U;
    ArenaStats arena_stats;
    return build_compact_arena(root, arena_config, &arena_stats, error);
}

bool build_semantic_arena(
    const std::filesystem::path& root,
    std::string* error) {
    LogicalNodeSourceStoreBinding binding;
    if (!inspect_logical_node_source_store_binding(root, &binding, error)) {
        return false;
    }

    LogicalNodeArenaBuildConfig config;
    config.candidate_commit =
        "0123456789abcdef0123456789abcdef01234567";
    config.candidate_tree =
        "89abcdef0123456789abcdef0123456789abcdef";
    config.source_sha256 = binding.payload_sha256;
    config.semantic_bucket_count = 64U;
    config.semantic_hash_bits = 64U;

    LogicalNodeArenaV2StoreBoundWriter writer(root, config);
    return writer.begin(error) &&
        writer.append_node(
            LogicalNodeInput{
                1U, 0U, 0U, 0U, kNoLogicalNodeOrdinal,
                "#document", "", "", 0U},
            {},
            error) &&
        writer.append_node(
            LogicalNodeInput{
                2U, 0U, 0U, 1U, 0U,
                "div", "main", "display:block", 0U},
            {},
            error) &&
        writer.append_node(
            LogicalNodeInput{
                3U, 0U, 1U, 6U, 1U,
                "#text", "", "", 0U},
            {},
            error) &&
        writer.append_node(
            LogicalNodeInput{
                4U, 1U, 1U, 2U, 1U,
                "span", "note", "display:inline", 0U},
            {},
            error) &&
        writer.finish(error);
}

bool build_fixture(
    const std::filesystem::path& root,
    bool include_record_index,
    std::string* error) {
    return build_store_and_layout_arena(root, error) &&
        build_semantic_arena(root, error) &&
        (!include_record_index || build_logical_node_record_index(root, error));
}

bool test_worker_lane_resolves_record_postings_to_semantics() {
    const std::filesystem::path root = unique_root("tab-record-semantics-worker");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_fixture(root, true, &error), error)) {
        return false;
    }

    ZenithTabRuntime runtime(root, nullptr, 4101U);
    if (!require(runtime.open(&error), error)) {
        return false;
    }

    ZenithRecordSemanticWindowResult first;
    if (!require(runtime.semantic_nodes_for_source_record_on_lane(
            FrameExecutionLane::Worker,
            0U,
            kNoLogicalNodeRecordPosting,
            1U,
            &first,
            &error), error) ||
        !require(first.source_record_index == 0U, "physical record identity") ||
        !require(first.nodes.size() == 1U, "first page node count") ||
        !require(first.nodes[0].source_overlap.node_ordinal == 1U,
                 "first posting node ordinal") ||
        !require(first.nodes[0].source_overlap.record_byte_offset == 0U &&
                     first.nodes[0].source_overlap.record_byte_length == 1U,
                 "first posting overlap") ||
        !require(first.nodes[0].semantic.tag == "div", "first semantic tag") ||
        !require(first.nodes[0].semantic.role == "main", "first semantic role") ||
        !require(first.truncated, "first page truncation") ||
        !require(first.next_posting_ordinal != kNoLogicalNodeRecordPosting,
                 "first page continuation")) {
        return false;
    }

    ZenithRecordSemanticWindowResult second;
    if (!require(runtime.semantic_nodes_for_source_record(
            0U,
            first.next_posting_ordinal,
            8U,
            &second,
            &error), error) ||
        !require(second.nodes.size() == 1U, "continuation node count") ||
        !require(second.nodes[0].source_overlap.node_ordinal == 2U,
                 "continuation node ordinal") ||
        !require(second.nodes[0].source_overlap.record_byte_offset == 1U &&
                     second.nodes[0].source_overlap.record_byte_length == 1U,
                 "cross-record text overlap") ||
        !require(second.nodes[0].semantic.tag == "#text",
                 "continuation semantic text tag") ||
        !require(!second.truncated, "continuation reaches head tail")) {
        return false;
    }

    const ZenithTabRuntimeStats& stats = runtime.stats();
    return require(stats.semantic_record_requests == 2U,
                   "semantic request telemetry") &&
        require(stats.semantic_record_successes == 2U,
                "semantic success telemetry") &&
        require(stats.semantic_nodes_materialized == 2U,
                "semantic materialization telemetry");
}

bool test_ui_lane_rejects_before_semantic_sidecar_open() {
    const std::filesystem::path root = unique_root("tab-record-semantics-ui");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_fixture(root, false, &error), error)) {
        return false;
    }

    ZenithTabRuntime runtime(root, nullptr, 4102U);
    if (!require(runtime.open(&error), error) ||
        !require(!std::filesystem::exists(root / "node-record-index-v1"),
                 "fixture intentionally has no record index")) {
        return false;
    }

    ZenithRecordSemanticWindowResult blocked;
    if (!require(!runtime.semantic_nodes_for_source_record_on_lane(
            FrameExecutionLane::Ui,
            0U,
            kNoLogicalNodeRecordPosting,
            1U,
            &blocked,
            &error),
            "UI semantic query rejected") ||
        !require(error.find("forbidden on the UI execution lane") !=
                     std::string::npos,
                 "UI rejection exposes lane fence") ||
        !require(blocked.nodes.empty(), "UI rejection returns no semantics") ||
        !require(!std::filesystem::exists(root / "node-record-index-v1"),
                 "UI rejection did not create/open a replacement index") ||
        !require(runtime.stats().ui_semantic_record_rejections == 1U,
                 "UI semantic rejection telemetry")) {
        return false;
    }

    error.clear();
    ZenithRecordSemanticWindowResult worker;
    return require(!runtime.semantic_nodes_for_source_record_on_lane(
               FrameExecutionLane::Worker,
               0U,
               kNoLogicalNodeRecordPosting,
               1U,
               &worker,
               &error),
            "worker query fails when authoritative index is absent") &&
        require(!error.empty(), "worker absence failure is explicit") &&
        require(runtime.stats().semantic_record_failures == 1U,
                "worker semantic failure telemetry");
}

} // namespace

int main() {
    if (!test_worker_lane_resolves_record_postings_to_semantics() ||
        !test_ui_lane_rejects_before_semantic_sidecar_open()) {
        return 1;
    }
    return 0;
}
