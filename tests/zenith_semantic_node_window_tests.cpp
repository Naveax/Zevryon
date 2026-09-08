#include "logical_node_arena_v2_store_bound.hpp"
#include "logical_node_source.hpp"
#include "massivedoc_store.hpp"
#include "zenith_semantic_node_window.hpp"

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

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::LogicalNodeArenaBuildConfig;
using zevryon::massivedoc::LogicalNodeArenaV2StoreBoundWriter;
using zevryon::massivedoc::LogicalNodeAttributeInput;
using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::LogicalNodeSourceStoreBinding;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::ZenithSemanticNodeWindow;
using zevryon::massivedoc::ZenithSemanticNodeWindowConfig;
using zevryon::massivedoc::ZenithSemanticNodeWindowResult;
using zevryon::massivedoc::inspect_logical_node_source_store_binding;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: zenith semantic node window: " << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path unique_root(std::string_view name) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("zevryon-") + std::string(name) + "-" + std::to_string(tick));
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
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

LogicalNodeArenaBuildConfig arena_config(
    const LogicalNodeSourceStoreBinding& binding) {
    LogicalNodeArenaBuildConfig config;
    config.candidate_commit = "0123456789abcdef0123456789abcdef01234567";
    config.candidate_tree = "89abcdef0123456789abcdef0123456789abcdef";
    config.source_sha256 = binding.payload_sha256;
    config.semantic_bucket_count = 64U;
    config.semantic_hash_bits = 64U;
    return config;
}

bool build_fixture(const std::filesystem::path& root, std::string* error) {
    const std::string payload(200U, 'x');
    StoreWriter store(root);
    if (!store.append(6001U, bytes(payload), error)) {
        return false;
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = payload.size();
    metadata.logical_records = 1U;
    metadata.logical_nodes = 4U;
    metadata.largest_record_bytes = payload.size();
    if (!store.finalize(metadata, nullptr, error)) {
        return false;
    }

    LogicalNodeSourceStoreBinding binding;
    if (!inspect_logical_node_source_store_binding(root, &binding, error)) {
        return false;
    }

    LogicalNodeArenaV2StoreBoundWriter writer(root, arena_config(binding));
    if (!writer.begin(error)) {
        return false;
    }
    const std::array<LogicalNodeAttributeInput, 2> root_attributes{{
        {"lang", "en", 0U},
        {"class", "page", 0U},
    }};
    if (!writer.append_node(
            LogicalNodeInput{
                1U, 0U, 0U, 100U, kNoLogicalNodeOrdinal,
                "html", "", "display:block", 1U},
            root_attributes,
            error)) {
        return false;
    }
    const std::array<LogicalNodeAttributeInput, 2> child_attributes{{
        {"class", "panel", 0U},
        {"data-empty", "", 7U},
    }};
    if (!writer.append_node(
            LogicalNodeInput{
                2U, 0U, 100U, 50U, 0U,
                "div", "main", "display:block", 2U},
            child_attributes,
            error) ||
        !writer.append_node(
            LogicalNodeInput{
                3U, 0U, 150U, 25U, 1U,
                "span", "text", "color:red", 3U},
            {},
            error)) {
        return false;
    }
    const std::array<LogicalNodeAttributeInput, 1> sibling_attributes{{
        {"class", "panel", 0U},
    }};
    return writer.append_node(
               LogicalNodeInput{
                   4U, 0U, 175U, 25U, 0U,
                   "div", "main", "display:block", 4U},
               sibling_attributes,
               error) &&
        writer.finish(error);
}

bool test_round_trip_and_node_count_windowing() {
    const std::filesystem::path root = unique_root("semantic-window-roundtrip");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_fixture(root, &error), error)) {
        return false;
    }

    ZenithSemanticNodeWindowConfig config;
    config.maximum_nodes = 2U;
    ZenithSemanticNodeWindow window(root, config);
    ZenithSemanticNodeWindowResult first;
    ZenithSemanticNodeWindowResult second;
    ZenithSemanticNodeWindowResult end;
    if (!require(window.open(&error), error) ||
        !require(window.read(0U, &first, &error), error) ||
        !require(first.nodes.size() == 2U, "first window node count") ||
        !require(first.truncated && first.next_ordinal == 2U, "first window continuation") ||
        !require(first.arena_node_count == 4U, "arena node count exposed") ||
        !require(first.attribute_count == 4U, "first window attribute count") ||
        !require(first.semantic_bytes == 72U, "first window exact semantic bytes") ||
        !require(first.nodes[0].tag == "html", "root tag") ||
        !require(first.nodes[0].role.empty(), "empty root role") ||
        !require(first.nodes[0].attributes.size() == 2U, "root attributes complete") ||
        !require(first.nodes[1].tag == "div" && first.nodes[1].role == "main", "child semantics") ||
        !require(first.nodes[1].record.parent_ordinal == 0U, "child topology") ||
        !require(first.nodes[1].attributes[1].name == "data-empty", "second attribute name") ||
        !require(first.nodes[1].attributes[1].value.empty(), "empty attribute value") ||
        !require(first.nodes[1].attributes[1].flags == 7U, "attribute flags") ||
        !require(window.read(first.next_ordinal, &second, &error), error) ||
        !require(second.nodes.size() == 2U, "second window node count") ||
        !require(!second.truncated && second.next_ordinal == 4U, "second window reaches end") ||
        !require(second.nodes[0].record.parent_ordinal == 1U, "grandchild topology") ||
        !require(second.nodes[1].record.next_sibling_ordinal == kNoLogicalNodeOrdinal,
                 "sibling topology complete") ||
        !require(window.read(4U, &end, &error), error) ||
        !require(end.nodes.empty() && !end.truncated && end.next_ordinal == 4U,
                 "end ordinal is a valid empty window") ||
        !require(!window.read(5U, &end, &error), "out-of-range ordinal rejected")) {
        return false;
    }
    return true;
}

bool test_total_attribute_budget_truncates_at_node_boundary() {
    const std::filesystem::path root = unique_root("semantic-window-attribute-budget");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_fixture(root, &error), error)) {
        return false;
    }
    ZenithSemanticNodeWindowConfig config;
    config.maximum_attributes_per_node = 2U;
    config.maximum_total_attributes = 2U;
    ZenithSemanticNodeWindow window(root, config);
    ZenithSemanticNodeWindowResult result;
    return require(window.open(&error), error) &&
        require(window.read(0U, &result, &error), error) &&
        require(result.nodes.size() == 1U, "attribute budget returns one complete node") &&
        require(result.attribute_count == 2U, "attribute budget exact count") &&
        require(result.truncated && result.next_ordinal == 1U,
                "attribute budget truncates before child");
}

bool test_semantic_budget_truncates_or_fails_closed() {
    const std::filesystem::path root = unique_root("semantic-window-semantic-budget");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_fixture(root, &error), error)) {
        return false;
    }
    ZenithSemanticNodeWindowConfig config;
    config.maximum_semantic_bytes = 35U;
    ZenithSemanticNodeWindow window(root, config);
    ZenithSemanticNodeWindowResult result;
    if (!require(window.open(&error), error) ||
        !require(window.read(0U, &result, &error), error) ||
        !require(result.nodes.size() == 1U && result.semantic_bytes == 32U,
                 "semantic budget keeps complete root") ||
        !require(result.truncated && result.next_ordinal == 1U,
                 "semantic budget truncates before oversized cumulative child") ||
        !require(!window.read(1U, &result, &error),
                 "single node exceeding semantic budget fails closed")) {
        return false;
    }
    return true;
}

bool test_per_node_attribute_budget_and_invalid_config_rejected() {
    const std::filesystem::path root = unique_root("semantic-window-invalid-budget");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_fixture(root, &error), error)) {
        return false;
    }

    ZenithSemanticNodeWindowConfig attribute_config;
    attribute_config.maximum_attributes_per_node = 1U;
    attribute_config.maximum_total_attributes = 1U;
    ZenithSemanticNodeWindow attribute_window(root, attribute_config);
    ZenithSemanticNodeWindowResult result;
    if (!require(attribute_window.open(&error), error) ||
        !require(!attribute_window.read(0U, &result, &error),
                 "per-node attribute overflow rejected")) {
        return false;
    }

    ZenithSemanticNodeWindowConfig invalid;
    invalid.maximum_nodes = ZenithSemanticNodeWindowConfig::kMaximumNodesLimit + 1U;
    ZenithSemanticNodeWindow invalid_window(root, invalid);
    return require(!invalid_window.open(&error), "hard config ceiling rejected");
}

} // namespace

int main() {
    if (!test_round_trip_and_node_count_windowing() ||
        !test_total_attribute_budget_truncates_at_node_boundary() ||
        !test_semantic_budget_truncates_or_fails_closed() ||
        !test_per_node_attribute_budget_and_invalid_config_rejected()) {
        return 1;
    }
    return 0;
}
