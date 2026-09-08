#include "logical_node_source_v2.hpp"
#include "massivedoc_store.hpp"
#include "streaming_html_node_source_v2.hpp"

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
using zevryon::massivedoc::LogicalNodeSourceNode;
using zevryon::massivedoc::LogicalNodeSourceV2Reader;
using zevryon::massivedoc::LogicalNodeSourceV2ValidationStats;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::produce_streaming_html_node_source_v2;
using zevryon::massivedoc::validate_logical_node_source_v2_against_store;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: streaming HTML v2 strict syntax: " << message << '\n';
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

bool build_store(
    const std::filesystem::path& root,
    std::string_view html,
    std::uint64_t logical_nodes,
    std::string* error) {
    StoreWriter writer(root);
    if (!writer.append(8301U, bytes(html), error)) {
        return false;
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = static_cast<std::uint64_t>(html.size());
    metadata.logical_records = 1U;
    metadata.logical_nodes = logical_nodes;
    metadata.largest_record_bytes = static_cast<std::uint64_t>(html.size());
    return writer.finalize(metadata, nullptr, error);
}

bool test_non_void_self_closing_syntax_fails_closed() {
    const std::filesystem::path root = unique_root("html-v2-nonvoid-self-close");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    if (!require(build_store(store_root, "<div/>", 2U, &error), error)) {
        return false;
    }

    return require(!produce_streaming_html_node_source_v2(
                       store_root, source_path, {}, nullptr, &error),
                   "non-void self-closing syntax is rejected") &&
        require(error.find("self-closing syntax on non-void") != std::string::npos,
                "rejection identifies unsupported non-void self-closing syntax") &&
        require(!std::filesystem::exists(source_path),
                "rejected non-void syntax cannot publish source") &&
        require(!std::filesystem::exists(
                    std::filesystem::path(source_path.string() + ".building")),
                "rejected non-void syntax leaves no building source");
}

bool test_void_self_closing_syntax_remains_supported() {
    const std::filesystem::path root = unique_root("html-v2-void-self-close");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    if (!require(build_store(store_root, "<br/>", 2U, &error), error) ||
        !require(produce_streaming_html_node_source_v2(
                     store_root, source_path, {}, nullptr, &error), error)) {
        return false;
    }

    LogicalNodeSourceV2Reader reader(source_path);
    LogicalNodeSourceNode document;
    LogicalNodeSourceNode element;
    LogicalNodeSourceNode extra;
    bool has_node = false;
    if (!require(reader.open(&error), error) ||
        !require(reader.next(&document, &has_node, &error) && has_node,
                 "document node is present") ||
        !require(reader.next(&element, &has_node, &error) && has_node,
                 "void element node is present") ||
        !require(reader.next(&extra, &has_node, &error) && !has_node,
                 "source contains exactly two nodes") ||
        !require(document.tag == "#document", "document semantic survives") ||
        !require(element.tag == "br", "void tag survives") ||
        !require(element.parent_ordinal == 0U, "void element parent survives") ||
        !require(element.source_record_index == 0U &&
                     element.source_byte_offset == 0U &&
                     element.source_byte_length == 5U,
                 "void element keeps exact source span")) {
        return false;
    }

    LogicalNodeSourceV2ValidationStats validation;
    return require(validate_logical_node_source_v2_against_store(
                       source_path, store_root, &validation, &error), error) &&
        require(validation.nodes_validated == 2U, "validator accepts document + br") &&
        require(validation.source_span_bytes_streamed == 5U,
                "validator streams exact void-element source span");
}

} // namespace

int main() {
    if (!test_non_void_self_closing_syntax_fails_closed() ||
        !test_void_self_closing_syntax_remains_supported()) {
        return 1;
    }
    return 0;
}
