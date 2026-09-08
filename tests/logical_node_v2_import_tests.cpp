#include "logical_node_arena_v2.hpp"
#include "logical_node_source.hpp"
#include "logical_node_source_v2.hpp"
#include "logical_node_v2_import.hpp"
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

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::LogicalNodeArenaV2Reader;
using zevryon::massivedoc::LogicalNodeAttributeInput;
using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::LogicalNodeRecord;
using zevryon::massivedoc::LogicalNodeSourceStoreBinding;
using zevryon::massivedoc::LogicalNodeSourceV2Writer;
using zevryon::massivedoc::LogicalNodeV2ImportConfig;
using zevryon::massivedoc::LogicalNodeV2ImportStats;
using zevryon::massivedoc::LogicalSemanticKind;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::import_logical_node_source_v2_to_arena_v2;
using zevryon::massivedoc::inspect_logical_node_source_store_binding;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: logical node v2 import: " << message << '\n';
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
        reinterpret_cast<const std::byte*>(text.data()),
        text.size());
}

bool build_store(const std::filesystem::path& root, std::string* error) {
    constexpr std::array<std::string_view, 3> records{{"ab", "cdef", "gh"}};
    StoreWriter writer(root);
    for (std::size_t index = 0U; index < records.size(); ++index) {
        if (!writer.append(
                7001U + static_cast<std::uint64_t>(index),
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

bool write_source(
    const std::filesystem::path& path,
    const LogicalNodeSourceStoreBinding& binding,
    std::uint64_t text_length,
    std::string* error) {
    LogicalNodeSourceV2Writer writer(path);
    if (!writer.begin(error) ||
        !writer.append_node(
            LogicalNodeInput{
                1U, 0U, 0U, 0U, kNoLogicalNodeOrdinal,
                "#document", "", "", 11U},
            {},
            error)) {
        return false;
    }
    const std::array<LogicalNodeAttributeInput, 1> attributes{{
        {"data-kind", "text", 9U},
    }};
    return writer.append_node(
               LogicalNodeInput{
                   2U, 0U, 1U, text_length, 0U,
                   "#text", "text", "white-space:normal", 12U},
               attributes,
               error) &&
        writer.finish(binding, error);
}

LogicalNodeV2ImportConfig import_config(std::uint32_t hash_bits = 64U) {
    LogicalNodeV2ImportConfig config;
    config.candidate_commit = "0123456789abcdef0123456789abcdef01234567";
    config.candidate_tree = "89abcdef0123456789abcdef0123456789abcdef";
    config.semantic_bucket_count = 64U;
    config.semantic_hash_bits = hash_bits;
    return config;
}

bool test_cross_record_import_round_trip() {
    const std::filesystem::path root = unique_root("node-v2-import-roundtrip");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    LogicalNodeSourceStoreBinding binding;
    if (!require(build_store(store_root, &error), error) ||
        !require(inspect_logical_node_source_store_binding(
            store_root, &binding, &error), error) ||
        !require(write_source(source_path, binding, 6U, &error), error)) {
        return false;
    }

    LogicalNodeV2ImportStats stats;
    if (!require(import_logical_node_source_v2_to_arena_v2(
            source_path, store_root, import_config(), &stats, &error), error) ||
        !require(stats.nodes_imported == 2U, "imports exact node count") ||
        !require(stats.attributes_imported == 1U, "imports exact attribute count") ||
        !require(stats.validation.source_span_bytes_streamed == 6U,
                 "validation streams exact source span bytes") ||
        !require(std::filesystem::exists(store_root / "node-arena-v2"),
                 "v2 arena published") ||
        !require(!std::filesystem::exists(store_root / "node-arena-v2.building"),
                 "no staging tree remains after publish")) {
        return false;
    }

    LogicalNodeArenaV2Reader arena(store_root);
    LogicalNodeRecord document;
    LogicalNodeRecord text;
    std::string semantic;
    if (!require(arena.open(&error), error) ||
        !require(arena.node_by_ordinal(0U, &document, &error), error) ||
        !require(arena.node_by_ordinal(1U, &text, &error), error) ||
        !require(text.parent_ordinal == 0U, "text parent topology") ||
        !require(text.source_record_index == 0U &&
                 text.source_byte_offset == 1U &&
                 text.source_byte_length == 6U,
                 "cross-record source triple survives arena import") ||
        !require(text.flags == 12U, "node flags survive import") ||
        !require(text.attribute_count == 1U, "attribute slice survives import") ||
        !require(arena.resolve_semantic(
            LogicalSemanticKind::tag, text.tag_id, &semantic, &error), error) ||
        !require(semantic == "#text", "text tag survives interning") ||
        !require(arena.resolve_semantic(
            LogicalSemanticKind::role, text.role_id, &semantic, &error), error) ||
        !require(semantic == "text", "role survives interning") ||
        !require(arena.resolve_semantic(
            LogicalSemanticKind::style, text.style_id, &semantic, &error), error) ||
        !require(semantic == "white-space:normal", "style survives interning")) {
        return false;
    }
    return true;
}

bool test_invalid_span_never_creates_arena_staging() {
    const std::filesystem::path root = unique_root("node-v2-import-invalid-span");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "invalid.zvnsrc";
    std::string error;
    LogicalNodeSourceStoreBinding binding;
    if (!require(build_store(store_root, &error), error) ||
        !require(inspect_logical_node_source_store_binding(
            store_root, &binding, &error), error) ||
        !require(write_source(source_path, binding, 8U, &error), error)) {
        return false;
    }

    LogicalNodeV2ImportStats stats;
    return require(!import_logical_node_source_v2_to_arena_v2(
            source_path, store_root, import_config(), &stats, &error),
            "escaping span rejected") &&
        require(!std::filesystem::exists(store_root / "node-arena-v2"),
                "invalid source cannot publish arena") &&
        require(!std::filesystem::exists(store_root / "node-arena-v2.building"),
                "validation failure occurs before arena staging");
}

bool test_binding_mismatch_and_invalid_config_fail_before_staging() {
    const std::filesystem::path root = unique_root("node-v2-import-binding");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "binding.zvnsrc";
    std::string error;
    LogicalNodeSourceStoreBinding binding;
    if (!require(build_store(store_root, &error), error) ||
        !require(inspect_logical_node_source_store_binding(
            store_root, &binding, &error), error)) {
        return false;
    }
    binding.record_sequence_sha256[0] ^= 0x80U;
    if (!require(write_source(source_path, binding, 6U, &error), error) ||
        !require(!import_logical_node_source_v2_to_arena_v2(
            source_path, store_root, import_config(), nullptr, &error),
            "binding mismatch rejected") ||
        !require(!std::filesystem::exists(store_root / "node-arena-v2.building"),
                 "binding mismatch cannot create staging")) {
        return false;
    }

    LogicalNodeV2ImportConfig invalid = import_config();
    invalid.semantic_hash_bits = 65U;
    return require(!import_logical_node_source_v2_to_arena_v2(
            source_path, store_root, invalid, nullptr, &error),
            "invalid import config rejected") &&
        require(!std::filesystem::exists(store_root / "node-arena-v2.building"),
                "invalid config cannot create staging");
}

bool test_forced_collision_configuration_remains_supported() {
    const std::filesystem::path root = unique_root("node-v2-import-collision");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "collision.zvnsrc";
    std::string error;
    LogicalNodeSourceStoreBinding binding;
    if (!require(build_store(store_root, &error), error) ||
        !require(inspect_logical_node_source_store_binding(
            store_root, &binding, &error), error) ||
        !require(write_source(source_path, binding, 6U, &error), error)) {
        return false;
    }
    return require(import_logical_node_source_v2_to_arena_v2(
        source_path, store_root, import_config(0U), nullptr, &error), error);
}

} // namespace

int main() {
    if (!test_cross_record_import_round_trip() ||
        !test_invalid_span_never_creates_arena_staging() ||
        !test_binding_mismatch_and_invalid_config_fail_before_staging() ||
        !test_forced_collision_configuration_remains_supported()) {
        return 1;
    }
    return 0;
}
