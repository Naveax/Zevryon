#include "logical_dom_traversal.hpp"

#include "logical_node_source.hpp"
#include "massivedoc_store.hpp"

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
        std::cerr << "FAILED: logical DOM traversal: " << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path unique_root() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("zevryon-logical-dom-traversal-") + std::to_string(tick));
}

struct Cleanup final {
    explicit Cleanup(std::filesystem::path value) : root(std::move(value)) {}
    ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    Cleanup(const Cleanup&) = delete;
    Cleanup& operator=(const Cleanup&) = delete;
    std::filesystem::path root;
};

std::span<const std::byte> bytes(std::string_view text) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

bool build_store(const std::filesystem::path& root, std::string* error) {
    constexpr std::string_view payload = "dom-tree";
    StoreWriter writer(root);
    if (!writer.append(7001U, bytes(payload), error)) {
        return false;
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = payload.size();
    metadata.logical_records = 1U;
    metadata.logical_nodes = 9U;
    metadata.largest_record_bytes = payload.size();
    return writer.finalize(metadata, nullptr, error);
}

bool append_node(
    LogicalNodeArenaV2StoreBoundWriter* writer,
    std::uint64_t logical_id,
    std::uint64_t parent,
    std::string_view tag,
    std::string* error) {
    return writer->append_node(
        LogicalNodeInput{
            logical_id,
            0U,
            0U,
            0U,
            parent,
            tag,
            "",
            "",
            0U},
        {},
        error);
}

bool build_arena(const std::filesystem::path& root, std::string* error) {
    LogicalNodeSourceStoreBinding binding;
    if (!inspect_logical_node_source_store_binding(root, &binding, error)) {
        return false;
    }
    LogicalNodeArenaBuildConfig config;
    config.candidate_commit = "0123456789abcdef0123456789abcdef01234567";
    config.candidate_tree = "89abcdef0123456789abcdef0123456789abcdef";
    config.source_sha256 = binding.payload_sha256;
    config.semantic_bucket_count = 64U;
    config.semantic_hash_bits = 64U;

    LogicalNodeArenaV2StoreBoundWriter writer(root, config);
    if (!writer.begin(error) ||
        !append_node(&writer, 1U, kNoLogicalNodeOrdinal, "#document", error) ||
        !append_node(&writer, 2U, 0U, "html", error) ||
        !append_node(&writer, 3U, 1U, "head", error) ||
        !append_node(&writer, 4U, 1U, "body", error) ||
        !append_node(&writer, 5U, 3U, "div", error) ||
        !append_node(&writer, 6U, 4U, "span", error) ||
        !append_node(&writer, 7U, 5U, "#text", error) ||
        !append_node(&writer, 8U, 3U, "p", error) ||
        !append_node(&writer, 9U, 7U, "#text", error)) {
        return false;
    }
    return writer.finish(error);
}

bool expect_found(
    const LogicalDomTraversalResult& result,
    std::uint64_t ordinal,
    std::string_view message) {
    return require(result.found, message) &&
        require(result.ordinal == ordinal, message) &&
        require(result.record.logical_id == ordinal + 1U, message);
}

bool test_exact_relations(const std::filesystem::path& root) {
    LogicalDomTraversal traversal(root);
    std::string error;
    if (!require(traversal.open(&error), error) ||
        !require(traversal.node_count() == 9U, "exact node count")) {
        return false;
    }

    LogicalDomTraversalResult result;
    if (!require(traversal.parent(0U, &result, &error), error) ||
        !require(!result.found, "document has no parent") ||
        !require(traversal.parent(6U, &result, &error), error) ||
        !expect_found(result, 5U, "text parent is span") ||
        !require(traversal.first_child(0U, &result, &error), error) ||
        !expect_found(result, 1U, "document first child is html") ||
        !require(traversal.first_child(2U, &result, &error), error) ||
        !require(!result.found, "head has no child") ||
        !require(traversal.next_sibling(2U, &result, &error), error) ||
        !expect_found(result, 3U, "head next sibling is body") ||
        !require(traversal.next_sibling(4U, &result, &error), error) ||
        !expect_found(result, 7U, "div next sibling is p") ||
        !require(traversal.next_sibling(7U, &result, &error), error) ||
        !require(!result.found, "p has no next sibling") ||
        !require(traversal.previous_sibling(3U, &result, &error), error) ||
        !expect_found(result, 2U, "body previous sibling is head") ||
        !require(traversal.previous_sibling(7U, &result, &error), error) ||
        !expect_found(result, 4U, "p previous sibling is div") ||
        !require(traversal.previous_sibling(4U, &result, &error), error) ||
        !require(!result.found, "div has no previous sibling")) {
        return false;
    }

    for (std::uint64_t ordinal = 0U; ordinal + 1U < 9U; ++ordinal) {
        if (!require(traversal.next_preorder(ordinal, &result, &error), error) ||
            !expect_found(result, ordinal + 1U, "forward preorder is exact")) {
            return false;
        }
    }
    if (!require(traversal.next_preorder(8U, &result, &error), error) ||
        !require(!result.found, "last node has no preorder successor")) {
        return false;
    }

    for (std::uint64_t ordinal = 1U; ordinal < 9U; ++ordinal) {
        if (!require(traversal.previous_preorder(ordinal, &result, &error), error) ||
            !expect_found(result, ordinal - 1U, "reverse preorder is exact")) {
            return false;
        }
    }
    if (!require(traversal.previous_preorder(0U, &result, &error), error) ||
        !require(!result.found, "document has no preorder predecessor")) {
        return false;
    }

    bool descendant = false;
    if (!require(traversal.is_strict_descendant(6U, 4U, &descendant, &error), error) ||
        !require(descendant, "nested text is a strict descendant of div") ||
        !require(traversal.is_strict_descendant(7U, 4U, &descendant, &error), error) ||
        !require(!descendant, "p is not a descendant of div") ||
        !require(traversal.is_strict_descendant(4U, 4U, &descendant, &error), error) ||
        !require(!descendant, "node is not its own strict descendant") ||
        !require(traversal.is_strict_descendant(8U, 3U, &descendant, &error), error) ||
        !require(descendant, "p text is a descendant of body")) {
        return false;
    }

    int order = 0;
    if (!require(traversal.compare_document_order(2U, 8U, &order, &error), error) ||
        !require(order == -1, "forward document order") ||
        !require(traversal.compare_document_order(8U, 2U, &order, &error), error) ||
        !require(order == 1, "reverse document order") ||
        !require(traversal.compare_document_order(5U, 5U, &order, &error), error) ||
        !require(order == 0, "identical document order") ||
        !require(!traversal.node(9U, &result, &error),
                 "out-of-range ordinal fails closed")) {
        return false;
    }
    return true;
}

bool test_hop_budget_and_config(const std::filesystem::path& root) {
    std::string error;
    LogicalDomTraversal tiny(root, LogicalDomTraversalConfig{1U});
    if (!require(tiny.open(&error), error)) {
        return false;
    }
    LogicalDomTraversalResult result;
    if (!require(
            !tiny.next_preorder(8U, &result, &error),
            "deep ascent exceeds one-hop traversal budget") ||
        !require(error.find("hop budget") != std::string::npos,
                 "hop-budget failure is explicit")) {
        return false;
    }

    LogicalDomTraversal invalid(root, LogicalDomTraversalConfig{0U});
    error.clear();
    return require(!invalid.open(&error), "zero-hop configuration fails closed") &&
        require(error.find("configuration") != std::string::npos,
                "invalid configuration failure is explicit");
}

} // namespace

int main() {
    const std::filesystem::path root = unique_root();
    Cleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, &error), error) ||
        !require(build_arena(root, &error), error) ||
        !test_exact_relations(root) ||
        !test_hop_budget_and_config(root)) {
        return 1;
    }
    std::cout << "logical DOM traversal authority passed\n";
    return 0;
}
