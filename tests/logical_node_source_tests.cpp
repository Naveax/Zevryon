#include "logical_node_arena.hpp"
#include "logical_node_source.hpp"
#include "massivedoc_store.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::LogicalNodeArenaReader;
using zevryon::massivedoc::LogicalNodeAttributeInput;
using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::LogicalNodeRecord;
using zevryon::massivedoc::LogicalNodeSourceImportConfig;
using zevryon::massivedoc::LogicalNodeSourceNode;
using zevryon::massivedoc::LogicalNodeSourceReader;
using zevryon::massivedoc::LogicalNodeSourceStoreBinding;
using zevryon::massivedoc::LogicalNodeSourceWriter;
using zevryon::massivedoc::LogicalSemanticKind;
using zevryon::massivedoc::StoreStats;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::import_logical_node_source_to_arena;
using zevryon::massivedoc::inspect_logical_node_source_store_binding;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;

constexpr std::uint64_t kSourceHeaderBytes = 112U;

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

bool build_store(
    const std::filesystem::path& store_root,
    StoreStats* stats,
    std::string* error) {
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

bool build_partition_store(
    const std::filesystem::path& store_root,
    std::string_view first,
    std::string_view second,
    std::string* error) {
    StoreWriter writer(store_root);
    if (!writer.append(2001U, bytes(first), error) ||
        !writer.append(2002U, bytes(second), error)) {
        return false;
    }
    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = first.size() + second.size();
    metadata.logical_records = 2U;
    metadata.logical_nodes = 1U;
    metadata.largest_record_bytes =
        first.size() > second.size() ? first.size() : second.size();
    return writer.finalize(metadata, nullptr, error);
}

bool binding_for(
    const std::filesystem::path& store_root,
    LogicalNodeSourceStoreBinding* binding,
    std::string* error) {
    return inspect_logical_node_source_store_binding(
        store_root,
        binding,
        error);
}

bool write_source(
    const std::filesystem::path& source_path,
    const LogicalNodeSourceStoreBinding& binding,
    bool escape_range,
    std::string* error) {
    LogicalNodeSourceWriter writer(source_path);
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
    return writer.finish(binding, error);
}

bool write_single_node_source(
    const std::filesystem::path& source_path,
    const LogicalNodeSourceStoreBinding& binding,
    std::string* error) {
    LogicalNodeSourceWriter writer(source_path);
    if (!writer.begin(error) ||
        !writer.append_node(
            LogicalNodeInput{
                1U,
                0U,
                0U,
                1U,
                kNoLogicalNodeOrdinal,
                "document",
                "",
                "",
                0U},
            {},
            error)) {
        return false;
    }
    return writer.finish(binding, error);
}

LogicalNodeSourceImportConfig import_config() {
    LogicalNodeSourceImportConfig config;
    config.candidate_commit =
        "0123456789abcdef0123456789abcdef01234567";
    config.candidate_tree =
        "89abcdef0123456789abcdef0123456789abcdef";
    config.semantic_bucket_count = 64U;
    config.semantic_hash_bits = 64U;
    return config;
}

std::uint32_t read_u32_le(std::span<const std::uint8_t> value) {
    std::uint32_t result = 0U;
    for (unsigned index = 0U; index < 4U; ++index) {
        result |= static_cast<std::uint32_t>(value[index]) << (index * 8U);
    }
    return result;
}

void store_u32_le(std::span<std::uint8_t> value, std::uint32_t number) {
    for (unsigned index = 0U; index < 4U; ++index) {
        value[index] = static_cast<std::uint8_t>(
            (number >> (index * 8U)) & 0xffU);
    }
}

std::uint32_t test_crc32(std::span<const std::uint8_t> value) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : value) {
        crc ^= static_cast<std::uint32_t>(byte);
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

bool tamper_frame_byte(
    const std::filesystem::path& path,
    std::uint64_t relative_offset) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        return false;
    }
    stream.seekg(
        static_cast<std::streamoff>(kSourceHeaderBytes + relative_offset),
        std::ios::beg);
    char value = 0;
    stream.read(&value, 1);
    if (!stream) {
        return false;
    }
    value = static_cast<char>(static_cast<unsigned char>(value) ^ 0x01U);
    stream.clear();
    stream.seekp(
        static_cast<std::streamoff>(kSourceHeaderBytes + relative_offset),
        std::ios::beg);
    stream.write(&value, 1);
    stream.flush();
    return static_cast<bool>(stream);
}

bool set_first_frame_attribute_count_with_valid_crc(
    const std::filesystem::path& path,
    std::uint32_t attribute_count) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        return false;
    }
    std::array<std::uint8_t, 4> size_bytes{};
    stream.seekg(static_cast<std::streamoff>(kSourceHeaderBytes), std::ios::beg);
    stream.read(
        reinterpret_cast<char*>(size_bytes.data()),
        static_cast<std::streamsize>(size_bytes.size()));
    if (!stream) {
        return false;
    }
    const std::uint32_t frame_bytes = read_u32_le(size_bytes);
    if (frame_bytes < 68U || frame_bytes > 16U * 1024U * 1024U) {
        return false;
    }
    std::vector<std::uint8_t> frame(frame_bytes);
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(kSourceHeaderBytes), std::ios::beg);
    stream.read(
        reinterpret_cast<char*>(frame.data()),
        static_cast<std::streamsize>(frame.size()));
    if (!stream) {
        return false;
    }
    store_u32_le(
        std::span<std::uint8_t>(frame.data() + 4U, 4U),
        attribute_count);
    const std::uint32_t crc = test_crc32(
        std::span<const std::uint8_t>(frame.data(), frame.size() - 4U));
    store_u32_le(
        std::span<std::uint8_t>(frame.data() + frame.size() - 4U, 4U),
        crc);
    stream.clear();
    stream.seekp(static_cast<std::streamoff>(kSourceHeaderBytes), std::ios::beg);
    stream.write(
        reinterpret_cast<const char*>(frame.data()),
        static_cast<std::streamsize>(frame.size()));
    stream.flush();
    return static_cast<bool>(stream);
}

bool test_round_trip_import_and_interning() {
    const std::filesystem::path root = unique_root("node-source-roundtrip");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    StoreStats stats;
    LogicalNodeSourceStoreBinding binding;
    if (!require(build_store(store_root, &stats, &error), error) ||
        !require(binding_for(store_root, &binding, &error), error) ||
        !require(write_source(source_path, binding, false, &error), error) ||
        !require(import_logical_node_source_to_arena(
            source_path,
            store_root,
            import_config(),
            &error),
            error)) {
        return false;
    }

    LogicalNodeArenaReader arena(store_root);
    LogicalNodeRecord first_child;
    LogicalNodeRecord sibling;
    std::string semantic;
    if (!require(arena.open(&error), error) ||
        !require(arena.manifest().node_count == 3U, "arena node count") ||
        !require(arena.manifest().attribute_count == 4U, "arena attribute count") ||
        !require(
            arena.manifest().source_sha256 == binding.payload_sha256,
            "arena source digest binding") ||
        !require(arena.node_by_ordinal(1U, &first_child, &error), error) ||
        !require(arena.node_by_ordinal(2U, &sibling, &error), error) ||
        !require(first_child.tag_id == sibling.tag_id, "repeated tag interned") ||
        !require(first_child.role_id == sibling.role_id, "repeated role interned") ||
        !require(first_child.style_id == sibling.style_id, "repeated style interned") ||
        !require(
            arena.resolve_semantic(
                LogicalSemanticKind::tag,
                first_child.tag_id,
                &semantic,
                &error),
            error) ||
        !require(semantic == "div", "semantic survives source-to-arena import") ||
        !require(
            first_child.source_record_index == 0U &&
                sibling.source_record_index == 1U,
            "physical source-record identities survive import")) {
        return false;
    }
    return true;
}

bool test_payload_hash_mismatch_fails_closed() {
    const std::filesystem::path root = unique_root("node-source-hash-mismatch");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    StoreStats stats;
    LogicalNodeSourceStoreBinding binding;
    if (!require(build_store(store_root, &stats, &error), error) ||
        !require(binding_for(store_root, &binding, &error), error)) {
        return false;
    }
    binding.payload_sha256[0] ^= 0x80U;
    if (!require(write_source(source_path, binding, false, &error), error) ||
        !require(
            !import_logical_node_source_to_arena(
                source_path,
                store_root,
                import_config(),
                &error),
            "mismatched payload digest rejected") ||
        !require(
            !std::filesystem::exists(store_root / "node-arena"),
            "payload mismatch cannot publish arena")) {
        return false;
    }
    return true;
}

bool test_record_partition_mismatch_fails_closed() {
    const std::filesystem::path root = unique_root("node-source-partition-mismatch");
    RootCleanup cleanup(root);
    const std::filesystem::path source_store = root / "source-store";
    const std::filesystem::path target_store = root / "target-store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    if (!require(build_partition_store(source_store, "ab", "c", &error), error) ||
        !require(build_partition_store(target_store, "a", "bc", &error), error)) {
        return false;
    }
    LogicalNodeSourceStoreBinding source_binding;
    LogicalNodeSourceStoreBinding target_binding;
    if (!require(binding_for(source_store, &source_binding, &error), error) ||
        !require(binding_for(target_store, &target_binding, &error), error) ||
        !require(
            source_binding.payload_sha256 == target_binding.payload_sha256,
            "partition oracle keeps the same concatenated payload SHA-256") ||
        !require(
            source_binding.record_sequence_sha256 !=
                target_binding.record_sequence_sha256,
            "partition oracle changes stable record-sequence identity") ||
        !require(write_single_node_source(source_path, source_binding, &error), error) ||
        !require(
            !import_logical_node_source_to_arena(
                source_path,
                target_store,
                import_config(),
                &error),
            "same payload with different record partition rejected") ||
        !require(
            !std::filesystem::exists(target_store / "node-arena"),
            "record partition mismatch cannot publish arena")) {
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
    LogicalNodeSourceStoreBinding binding;
    if (!require(build_store(store_root, &stats, &error), error) ||
        !require(binding_for(store_root, &binding, &error), error) ||
        !require(write_source(source_path, binding, true, &error), error) ||
        !require(
            !import_logical_node_source_to_arena(
                source_path,
                store_root,
                import_config(),
                &error),
            "record-escaping source range rejected") ||
        !require(
            !std::filesystem::exists(store_root / "node-arena"),
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
    LogicalNodeSourceStoreBinding binding;
    if (!require(build_store(store_root, &stats, &error), error) ||
        !require(binding_for(store_root, &binding, &error), error) ||
        !require(write_source(source_path, binding, false, &error), error) ||
        !require(tamper_frame_byte(source_path, 8U), "tamper first node frame")) {
        return false;
    }

    LogicalNodeSourceReader reader(source_path);
    LogicalNodeSourceNode node;
    bool has_node = false;
    if (!require(reader.open(&error), error) ||
        !require(
            !reader.next(&node, &has_node, &error),
            "node frame CRC tamper rejected")) {
        return false;
    }
    return true;
}

bool test_malicious_attribute_count_rejected_before_reserve() {
    const std::filesystem::path root = unique_root("node-source-attribute-count");
    RootCleanup cleanup(root);
    const std::filesystem::path store_root = root / "store";
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    StoreStats stats;
    LogicalNodeSourceStoreBinding binding;
    if (!require(build_store(store_root, &stats, &error), error) ||
        !require(binding_for(store_root, &binding, &error), error) ||
        !require(write_source(source_path, binding, false, &error), error) ||
        !require(
            set_first_frame_attribute_count_with_valid_crc(
                source_path,
                0xffffffffU),
            "forge oversized attribute count with valid frame CRC")) {
        return false;
    }

    LogicalNodeSourceReader reader(source_path);
    LogicalNodeSourceNode node;
    bool has_node = false;
    if (!require(reader.open(&error), error) ||
        !require(
            !reader.next(&node, &has_node, &error),
            "malicious attribute count rejected before allocation")) {
        return false;
    }
    return true;
}

bool test_unfinished_writer_cleans_build_file() {
    const std::filesystem::path root = unique_root("node-source-cleanup");
    RootCleanup cleanup(root);
    const std::filesystem::path source_path = root / "nodes.zvnsrc";
    std::string error;
    {
        LogicalNodeSourceWriter writer(source_path);
        if (!require(writer.begin(&error), error) ||
            !require(
                !writer.append_node(
                    LogicalNodeInput{
                        1U,
                        0U,
                        0U,
                        1U,
                        kNoLogicalNodeOrdinal,
                        "",
                        "",
                        "",
                        0U},
                    {},
                    &error),
                "invalid semantic node rejected")) {
            return false;
        }
    }
    std::filesystem::path building = source_path;
    building += ".building";
    return require(
        !std::filesystem::exists(source_path) &&
            !std::filesystem::exists(building),
        "unfinished writer leaves no published or building file");
}

} // namespace

int main() {
    if (!test_round_trip_import_and_interning() ||
        !test_payload_hash_mismatch_fails_closed() ||
        !test_record_partition_mismatch_fails_closed() ||
        !test_source_range_escape_fails_closed() ||
        !test_frame_crc_tamper_rejected() ||
        !test_malicious_attribute_count_rejected_before_reserve() ||
        !test_unfinished_writer_cleans_build_file()) {
        return 1;
    }
    return 0;
}
