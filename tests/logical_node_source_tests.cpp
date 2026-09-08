#include "logical_node_arena.hpp"
#include "logical_node_source.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::LogicalNodeArenaReader;
using zevryon::massivedoc::LogicalNodeAttributeInput;
using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::LogicalNodeRecord;
using zevryon::massivedoc::LogicalNodeSourceImportConfig;
using zevryon::massivedoc::LogicalNodeSourceNode;
using zevryon::massivedoc::LogicalNodeSourceReader;
using zevryon::massivedoc::LogicalNodeSourceWriter;
using zevryon::massivedoc::LogicalSemanticKind;
using zevryon::massivedoc::StoreStats;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::import_logical_node_source_to_arena;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: logical node source: " << message << '\n';
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

bool parse_digest(
    std::string_view text,
    std::array<std::uint8_t, 32>* digest) {
    if (digest == nullptr || text.size() != 64U) {
        return false;
    }
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0U; index < digest->size(); ++index) {
        const int high = nibble(text[index * 2U]);
        const int low = nibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        (*digest)[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool build_store(const std::filesystem::path& store_root, StoreStats* stats, std::string* error) {
    constexpr std::string_view first = "<div>hello</div>";
    constexpr std::string_view second = "world";
    StoreWriter writer(store_root);
    if (!writer.append(1001U, bytes(first), error) ||
        !writer.append(1002U, bytes(second), error)) {
        return false;
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = first.size() + second.size();
    metadata.logical_records = 2U;
    metadata.logical_nodes = 3U;
    metadata.style_runs = 1U;
    metadata.largest_record_bytes = first.size();
    return writer.finalize(metadata, stats, error);
}

bool write_source(
    const std::filesystem::path& source_path,
    const std::array<std::uint8_t, 32>& digest,
    bool escape_range,
    std::string* error) {
    LogicalNodeSourceWriter writer(source_path, digest);
    if (!writer.begin(error)) {
        return false;
    }
    const std::array<LogicalNodeAttributeInput, 2> root_attributes{{
        {"lang", "en", 0U},
        {"class", "page", 0U},
    }};
    if (!writer.append_node(
            LogicalNodeInput{
                1U,
                0U,
                0U,
                16U,
                kNoLogicalNodeOrdinal,
                "document",
                "",
                "display:block",
                1U},
            root_attributes,
            error)) {
        return false;
    }
    const std::array<LogicalNodeAttributeInput, 1> child_attributes{{
        {"class", "panel", 0U},
    }};
    if (!writer.append_node(
            LogicalNodeInput{
                2U,
                0U,
                5U,
                5U,
                0U,
                "div",
                "main",
                "display:block",
                2U},
            child_attributes,
            error)) {
        return false;
    }
    if (!writer.append_node(
            LogicalNodeInput{
                3U,
                1U,
                0U,
                escape_range ? 6U : 5U,
                0U,
                "div",
                "main",
                "display:block",
                3U},
            child_attributes,
            error)) {
        return false;
    }
    return writer.finish(error);
}

LogicalNodeSourceImportConfig import_config() {
    LogicalNodeSourceImportConfig config;
    config.candidate_commit = "0123456789abcdef0123456789abcdef01234567";
    config.candidate_tree = "89abcdef0123456789abcdef0123456789abcdef";
    config.semantic_bucket_count = 64U;
    config.semantic_hash_bits = 64U;
    return config;
}

bool test_round_trip_import_and_interning() {
    const std::filesystem::path root = unique_root("node-source-roundtrip");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    StoreStats stats;
    if (!require(build_store(store_root, &stats, &error), error)) {
        return false;
    }
    std::array<std::uint8_t, 32> digest{};
    if (!require(parse_digest(stats.payload_sha256, &digest), "parse store digest") ||
        !require(write_source(source_path, digest, false, &error), error) ||
        !require(import_logical_node_source_to_arena(
            source_path, store_root, import_config(), &error), error)) {
        return false;
    }

    LogicalNodeArenaReader arena(store_root);
    LogicalNodeRecord first_child;
    LogicalNodeRecord sibling;
    std::string semantic;
    if (!require(arena.open(&error), error) ||
        !require(arena.manifest().node_count == 3U, "arena node count") ||
        !require(arena.manifest().attribute_count == 4U, "arena attribute count") ||
        !require(arena.manifest().source_sha256 == digest, "arena source digest binding") ||
        !require(arena.node_by_ordinal(1U, &first_child, &error), error) ||
        !require(arena.node_by_ordinal(2U, &sibling, &error), error) ||
        !require(first_child.tag_id == sibling.tag_id, "repeated tag interned") ||
        !require(first_child.role_id == sibling.role_id, "repeated role interned") ||
        !require(first_child.style_id == sibling.style_id, "repeated style interned") ||
        !require(arena.resolve_semantic(
            LogicalSemanticKind::tag, first_child.tag_id, &semantic, &error), error) ||
        !require(semantic == "div", "semantic survives source-to-arena import") ||
        !require(first_child.source_record_index == 0U && sibling.source_record_index == 1U,
                 "physical source-record identities survive import")) {
        return false;
    }
    return true;
}

bool test_hash_mismatch_fails_closed() {
    const std::filesystem::path root = unique_root("node-source-hash-mismatch");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    StoreStats stats;
    if (!require(build_store(store_root, &stats, &error), error)) {
        return false;
    }
    std::array<std::uint8_t, 32> digest{};
    if (!require(parse_digest(stats.payload_sha256, &digest), "parse store digest")) {
        return false;
    }
    digest[0] ^= 0x80U;
    if (!require(write_source(source_path, digest, false, &error), error) ||
        !require(!import_logical_node_source_to_arena(
            source_path, store_root, import_config(), &error),
            "mismatched source digest rejected") ||
        !require(!std::filesystem::exists(store_root / "node-arena"),
                 "hash mismatch cannot publish arena")) {
        return false;
    }
    return true;
}

bool test_source_range_escape_fails_closed() {
    const std::filesystem::path root = unique_root("node-source-range-escape");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    StoreStats stats;
    if (!require(build_store(store_root, &stats, &error), error)) {
        return false;
    }
    std::array<std::uint8_t, 32> digest{};
    if (!require(parse_digest(stats.payload_sha256, &digest), "parse store digest") ||
        !require(write_source(source_path, digest, true, &error), error) ||
        !require(!import_logical_node_source_to_arena(
            source_path, store_root, import_config(), &error),
            "record-escaping source range rejected") ||
        !require(!std::filesystem::exists(store_root / "node-arena"),
                 "range failure cannot publish arena")) {
        return false;
    }
    return true;
}

bool test_frame_crc_tamper_rejected() {
    const std::filesystem::path root = unique_root("node-source-crc");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    StoreStats stats;
    if (!require(build_store(store_root, &stats, &error), error)) {
        return false;
    }
    std::array<std::uint8_t, 32> digest{};
    if (!require(parse_digest(stats.payload_sha256, &digest), "parse store digest") ||
        !require(write_source(source_path, digest, false, &error), error)) {
        return false;
    }
    {
        std::fstream stream(source_path, std::ios::binary | std::ios::in | std::ios::out);
        if (!require(static_cast<bool>(stream), "open source for tamper")) {
            return false;
        }
        stream.seekg(80, std::ios::beg);
        char value = 0;
        stream.read(&value, 1);
        if (!require(static_cast<bool>(stream), "read source tamper byte")) {
            return false;
        }
        value = static_cast<char>(static_cast<unsigned char>(value) ^ 0x01U);
        stream.clear();
        stream.seekp(80, std::ios::beg);
        stream.write(&value, 1);
        stream.flush();
        if (!require(static_cast<bool>(stream), "write source tamper byte")) {
            return false;
        }
    }

    LogicalNodeSourceReader reader(source_path);
    LogicalNodeSourceNode node;
    bool has_node = false;
    if (!require(reader.open(&error), error) ||
        !require(!reader.next(&node, &has_node, &error), "node frame CRC tamper rejected")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_round_trip_import_and_interning() ||
        !test_hash_mismatch_fails_closed() ||
        !test_source_range_escape_fails_closed() ||
        !test_frame_crc_tamper_rejected()) {
        return 1;
    }
    return 0;
}
