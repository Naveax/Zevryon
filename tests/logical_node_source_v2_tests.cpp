#include "logical_node_source.hpp"
#include "logical_node_source_v2.hpp"
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

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::LogicalNodeAttributeInput;
using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::LogicalNodeSourceNode;
using zevryon::massivedoc::LogicalNodeSourceReader;
using zevryon::massivedoc::LogicalNodeSourceStoreBinding;
using zevryon::massivedoc::LogicalNodeSourceV2Reader;
using zevryon::massivedoc::LogicalNodeSourceV2ValidationStats;
using zevryon::massivedoc::LogicalNodeSourceV2Writer;
using zevryon::massivedoc::LogicalNodeSourceWriter;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::inspect_logical_node_source_store_binding;
using zevryon::massivedoc::kLogicalNodeSourceV2FormatVersion;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;
using zevryon::massivedoc::validate_logical_node_source_v2_against_store;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: logical node source v2: " << message << '\n';
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

bool build_three_record_store(
    const std::filesystem::path& root,
    std::string* error) {
    constexpr std::array<std::string_view, 3> records{{"ab", "cdef", "gh"}};
    StoreWriter writer(root);
    for (std::size_t index = 0U; index < records.size(); ++index) {
        if (!writer.append(
                9001U + static_cast<std::uint64_t>(index),
                bytes(records[index]),
                error)) {
            return false;
        }
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = 8U;
    metadata.logical_records = 3U;
    metadata.logical_nodes = 2U;
    metadata.largest_record_bytes = 4U;
    return writer.finalize(metadata, nullptr, error);
}

bool write_v2_source(
    const std::filesystem::path& path,
    const LogicalNodeSourceStoreBinding& binding,
    std::uint64_t text_length,
    std::string* error) {
    LogicalNodeSourceV2Writer writer(path);
    if (!writer.begin(error) ||
        !writer.append_node(
            LogicalNodeInput{
                1U,
                0U,
                0U,
                0U,
                kNoLogicalNodeOrdinal,
                "#document",
                "",
                "",
                0U},
            {},
            error)) {
        return false;
    }
    const std::array<LogicalNodeAttributeInput, 1> attributes{{
        {"data-kind", "text", 0U},
    }};
    if (!writer.append_node(
            LogicalNodeInput{
                2U,
                0U,
                1U,
                text_length,
                0U,
                "#text",
                "",
                "",
                0U},
            attributes,
            error)) {
        return false;
    }
    return writer.finish(binding, error);
}

bool test_v2_round_trip_and_cross_record_validation() {
    const std::filesystem::path root = unique_root("node-source-v2-roundtrip");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes-v2.zvnsrc";
    std::string error;
    LogicalNodeSourceStoreBinding binding;
    if (!require(build_three_record_store(store_root, &error), error) ||
        !require(
            inspect_logical_node_source_store_binding(
                store_root, &binding, &error),
            error) ||
        !require(write_v2_source(source_path, binding, 6U, &error), error)) {
        return false;
    }

    LogicalNodeSourceV2Reader reader(source_path);
    LogicalNodeSourceNode root_node;
    LogicalNodeSourceNode text_node;
    bool has_node = false;
    if (!require(reader.open(&error), error) ||
        !require(
            reader.manifest().format_version == kLogicalNodeSourceV2FormatVersion,
            "manifest exposes v2") ||
        !require(reader.next(&root_node, &has_node, &error), error) ||
        !require(has_node && root_node.logical_id == 1U, "root round trips") ||
        !require(reader.next(&text_node, &has_node, &error), error) ||
        !require(has_node && text_node.logical_id == 2U, "text node round trips") ||
        !require(text_node.tag == "#text", "text semantic round trips") ||
        !require(
            text_node.source_record_index == 0U &&
                text_node.source_byte_offset == 1U &&
                text_node.source_byte_length == 6U,
            "cross-record span triple round trips") ||
        !require(
            text_node.attributes.size() == 1U &&
                text_node.attributes[0].name == "data-kind" &&
                text_node.attributes[0].value == "text",
            "v2 attributes round trip") ||
        !require(reader.next(&text_node, &has_node, &error), error) ||
        !require(!has_node, "reader reaches exact manifest end")) {
        return false;
    }

    LogicalNodeSourceV2ValidationStats stats;
    if (!require(
            validate_logical_node_source_v2_against_store(
                source_path, store_root, &stats, &error),
            error) ||
        !require(stats.nodes_validated == 2U, "validator counts nodes") ||
        !require(stats.attributes_validated == 1U, "validator counts attributes") ||
        !require(
            stats.source_span_bytes_streamed == 6U,
            "validator streams exact cross-record byte length") ||
        !require(stats.zero_length_spans == 1U, "document zero-span is validated")) {
        return false;
    }
    return true;
}

bool test_cross_record_escape_fails_closed() {
    const std::filesystem::path root = unique_root("node-source-v2-escape");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "escape.zvnsrc";
    std::string error;
    LogicalNodeSourceStoreBinding binding;
    if (!require(build_three_record_store(store_root, &error), error) ||
        !require(
            inspect_logical_node_source_store_binding(
                store_root, &binding, &error),
            error) ||
        !require(write_v2_source(source_path, binding, 8U, &error), error)) {
        return false;
    }

    LogicalNodeSourceV2ValidationStats stats;
    if (!require(
            !validate_logical_node_source_v2_against_store(
                source_path, store_root, &stats, &error),
            "span escaping final record is rejected") ||
        !require(
            error.find("escapes") != std::string::npos,
            "escape failure is explicit")) {
        return false;
    }
    return true;
}

bool test_v1_and_v2_remain_explicitly_separate() {
    const std::filesystem::path root = unique_root("node-source-version-isolation");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path v1_path = root / "v1.zvnsrc";
    std::string error;
    LogicalNodeSourceStoreBinding binding;
    if (!require(build_three_record_store(store_root, &error), error) ||
        !require(
            inspect_logical_node_source_store_binding(
                store_root, &binding, &error),
            error)) {
        return false;
    }

    LogicalNodeSourceWriter writer(v1_path);
    if (!require(writer.begin(&error), error) ||
        !require(
            writer.append_node(
                LogicalNodeInput{
                    1U,
                    0U,
                    0U,
                    0U,
                    kNoLogicalNodeOrdinal,
                    "#document",
                    "",
                    "",
                    0U},
                {},
                &error),
            error) ||
        !require(
            writer.append_node(
                LogicalNodeInput{
                    2U,
                    0U,
                    1U,
                    1U,
                    0U,
                    "span",
                    "",
                    "",
                    0U},
                {},
                &error),
            error) ||
        !require(writer.finish(binding, &error), error)) {
        return false;
    }

    LogicalNodeSourceReader v1_reader(v1_path);
    LogicalNodeSourceV2Reader v2_reader(v1_path);
    if (!require(v1_reader.open(&error), "admitted v1 reader still opens v1") ||
        !require(!v2_reader.open(&error), "v2 reader rejects a v1 header")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_v2_round_trip_and_cross_record_validation() ||
        !test_cross_record_escape_fails_closed() ||
        !test_v1_and_v2_remain_explicitly_separate()) {
        return 1;
    }
    std::cout << "Logical node source v2 tests passed\n";
    return 0;
}
