#include "logical_node_arena.hpp"
#include "logical_node_arena_v2_store_bound.hpp"
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
#include <utility>

namespace {

using zevryon::massivedoc::CorpusMetadata;
using zevryon::massivedoc::LogicalNodeArenaBuildConfig;
using zevryon::massivedoc::LogicalNodeArenaReader;
using zevryon::massivedoc::LogicalNodeArenaV2StoreBoundReader;
using zevryon::massivedoc::LogicalNodeArenaV2StoreBoundWriter;
using zevryon::massivedoc::LogicalNodeAttributeInput;
using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::LogicalNodeRecord;
using zevryon::massivedoc::LogicalNodeSourceStoreBinding;
using zevryon::massivedoc::LogicalSemanticKind;
using zevryon::massivedoc::StoreWriter;
using zevryon::massivedoc::inspect_logical_node_source_store_binding;
using zevryon::massivedoc::kLogicalNodeArenaV2FormatVersion;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: logical node arena v2: " << message << '\n';
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
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

bool build_store(
    const std::filesystem::path& root,
    bool alternate_partition,
    std::string* error) {
    constexpr std::array<std::string_view, 3> partition_a{{"ab", "cdef", "gh"}};
    constexpr std::array<std::string_view, 2> partition_b{{"abc", "defgh"}};

    StoreWriter writer(root);
    std::uint64_t largest = 0U;
    if (alternate_partition) {
        for (std::size_t index = 0U; index < partition_b.size(); ++index) {
            if (!writer.append(
                    9101U + static_cast<std::uint64_t>(index),
                    bytes(partition_b[index]),
                    error)) {
                return false;
            }
            largest = std::max<std::uint64_t>(largest, partition_b[index].size());
        }
    } else {
        for (std::size_t index = 0U; index < partition_a.size(); ++index) {
            if (!writer.append(
                    9101U + static_cast<std::uint64_t>(index),
                    bytes(partition_a[index]),
                    error)) {
                return false;
            }
            largest = std::max<std::uint64_t>(largest, partition_a[index].size());
        }
    }

    CorpusMetadata metadata;
    metadata.logical_utf8_bytes = 8U;
    metadata.logical_records = alternate_partition ? partition_b.size() : partition_a.size();
    metadata.logical_nodes = 2U;
    metadata.largest_record_bytes = largest;
    return writer.finalize(metadata, nullptr, error);
}

bool arena_config(
    const std::filesystem::path& root,
    LogicalNodeArenaBuildConfig* config,
    LogicalNodeSourceStoreBinding* binding,
    std::string* error) {
    if (!inspect_logical_node_source_store_binding(root, binding, error)) {
        return false;
    }
    config->candidate_commit =
        "0123456789abcdef0123456789abcdef01234567";
    config->candidate_tree =
        "89abcdef0123456789abcdef0123456789abcdef";
    config->source_sha256 = binding->payload_sha256;
    config->semantic_bucket_count = 64U;
    config->semantic_hash_bits = 64U;
    return true;
}

bool build_arena(const std::filesystem::path& root, std::string* error) {
    LogicalNodeArenaBuildConfig config;
    LogicalNodeSourceStoreBinding binding;
    if (!arena_config(root, &config, &binding, error)) {
        return false;
    }
    LogicalNodeArenaV2StoreBoundWriter writer(root, config);
    if (!writer.begin(error)) {
        return false;
    }
    const std::array<LogicalNodeAttributeInput, 1> root_attributes{{
        {"lang", "en", 0U},
    }};
    if (!writer.append_node(
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
            root_attributes,
            error) ||
        !writer.append_node(
            LogicalNodeInput{
                2U,
                1U,
                0U,
                4U,
                0U,
                "#text",
                "",
                "",
                0U},
            {},
            error)) {
        return false;
    }
    return writer.finish(error);
}

bool flip_byte(const std::filesystem::path& path, std::uint64_t offset) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    char value = 0;
    stream.read(&value, 1);
    if (!stream) {
        return false;
    }
    value = static_cast<char>(static_cast<unsigned char>(value) ^ 0x01U);
    stream.clear();
    stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.write(&value, 1);
    stream.flush();
    return static_cast<bool>(stream);
}

bool copy_file_bytes(
    const std::filesystem::path& from,
    const std::filesystem::path& to) {
    std::ifstream input(from, std::ios::binary);
    std::ofstream output(to, std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        return false;
    }
    output << input.rdbuf();
    output.flush();
    return static_cast<bool>(input) && static_cast<bool>(output);
}

bool copy_tree(
    const std::filesystem::path& from,
    const std::filesystem::path& to) {
    std::error_code error;
    std::filesystem::copy(
        from,
        to,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing,
        error);
    return !error;
}

bool test_round_trip_and_v1_isolation() {
    const std::filesystem::path root = unique_root("node-arena-v2-roundtrip");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, false, &error), error) ||
        !require(build_arena(root, &error), error) ||
        !require(std::filesystem::is_directory(root / "node-arena-v2"),
                 "v2 authoritative directory exists") ||
        !require(std::filesystem::is_regular_file(
                     root / "node-arena-v2" / "source-binding-v2.bin"),
                 "source binding is atomically published") ||
        !require(!std::filesystem::exists(root / "node-arena-v2.building"),
                 "v2 staging directory is gone after publication") ||
        !require(!std::filesystem::exists(root / "node-arena"),
                 "v2 publication does not create a v1 authoritative root")) {
        return false;
    }

    {
        LogicalNodeArenaReader old_reader(root);
        if (!require(!old_reader.open(&error),
                     "v1 reader cannot silently open isolated v2 arena")) {
            return false;
        }
    }

    LogicalNodeSourceStoreBinding expected;
    if (!require(
            inspect_logical_node_source_store_binding(root, &expected, &error),
            error)) {
        return false;
    }

    LogicalNodeRecord text;
    std::string semantic;
    LogicalNodeArenaV2StoreBoundReader reader(root);
    if (!require(reader.open(&error), error) ||
        !require(reader.manifest().format_version == kLogicalNodeArenaV2FormatVersion,
                 "wrapper exposes v2 format") ||
        !require(reader.manifest().storage_manifest.node_count == 2U,
                 "nested storage node count") ||
        !require(reader.manifest().storage_manifest.attribute_count == 1U,
                 "nested storage attribute count") ||
        !require(reader.source_binding().source_record_count == expected.source_record_count,
                 "physical record count binding") ||
        !require(reader.source_binding().record_sequence_sha256 == expected.record_sequence_sha256,
                 "physical record sequence binding") ||
        !require(reader.node_by_ordinal(1U, &text, &error), error) ||
        !require(text.source_record_index == 1U &&
                     text.source_byte_offset == 0U &&
                     text.source_byte_length == 4U,
                 "cross-record-sensitive source triple survives disk round trip") ||
        !require(reader.resolve_semantic(
                     LogicalSemanticKind::tag,
                     text.tag_id,
                     &semantic,
                     &error),
                 error) ||
        !require(semantic == "#text", "text semantic survives v2 wrapper")) {
        return false;
    }
    return true;
}

bool test_create_only_publication() {
    const std::filesystem::path root = unique_root("node-arena-v2-create-only");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, false, &error), error) ||
        !require(build_arena(root, &error), error)) {
        return false;
    }
    LogicalNodeArenaBuildConfig config;
    LogicalNodeSourceStoreBinding binding;
    if (!require(arena_config(root, &config, &binding, &error), error)) {
        return false;
    }
    LogicalNodeArenaV2StoreBoundWriter second(root, config);
    return require(!second.begin(&error),
                   "second v2 writer cannot overwrite authoritative arena");
}

bool test_binding_crc_tamper_fails_closed() {
    const std::filesystem::path root = unique_root("node-arena-v2-binding-crc");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, false, &error), error) ||
        !require(build_arena(root, &error), error) ||
        !require(flip_byte(root / "node-arena-v2" / "source-binding-v2.bin", 16U),
                 "binding CRC tamper fixture")) {
        return false;
    }
    LogicalNodeArenaV2StoreBoundReader reader(root);
    return require(!reader.open(&error), "binding with invalid CRC is rejected") &&
        require(error.find("CRC") != std::string::npos,
                "binding tamper reaches CRC authority");
}

bool test_marker_crc_tamper_fails_closed() {
    const std::filesystem::path root = unique_root("node-arena-v2-marker-crc");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_store(root, false, &error), error) ||
        !require(build_arena(root, &error), error) ||
        !require(flip_byte(root / "node-arena-v2" / "manifest-v2.bin", 32U),
                 "marker CRC tamper fixture")) {
        return false;
    }
    LogicalNodeArenaV2StoreBoundReader reader(root);
    return require(!reader.open(&error), "marker with invalid CRC is rejected") &&
        require(error.find("CRC") != std::string::npos,
                "marker payload tamper reaches CRC authority");
}

bool test_valid_crc_partition_frankenstein_fails_closed() {
    const std::filesystem::path root_a = unique_root("node-arena-v2-partition-a");
    const std::filesystem::path root_b = unique_root("node-arena-v2-partition-b");
    RootCleanup cleanup_a(root_a);
    RootCleanup cleanup_b(root_b);
    std::string error;
    if (!require(build_store(root_a, false, &error), error) ||
        !require(build_store(root_b, true, &error), error) ||
        !require(build_arena(root_a, &error), error) ||
        !require(build_arena(root_b, &error), error)) {
        return false;
    }

    LogicalNodeSourceStoreBinding binding_a;
    LogicalNodeSourceStoreBinding binding_b;
    if (!require(inspect_logical_node_source_store_binding(root_a, &binding_a, &error), error) ||
        !require(inspect_logical_node_source_store_binding(root_b, &binding_b, &error), error) ||
        !require(binding_a.payload_sha256 == binding_b.payload_sha256,
                 "fixture preserves exact payload SHA") ||
        !require(binding_a.record_sequence_sha256 != binding_b.record_sequence_sha256,
                 "fixture changes physical record sequence")) {
        return false;
    }

    const std::filesystem::path saved_binding = root_b / "saved-binding-v2.bin";
    if (!require(copy_file_bytes(
            root_b / "node-arena-v2" / "source-binding-v2.bin", saved_binding),
            "save valid binding from alternate partition")) {
        return false;
    }

    std::error_code fs_error;
    std::filesystem::remove_all(root_b / "node-arena-v2", fs_error);
    if (!require(!fs_error, "clear alternate arena") ||
        !require(copy_tree(root_a / "node-arena-v2", root_b / "node-arena-v2"),
                 "transplant complete arena from first partition") ||
        !require(copy_file_bytes(saved_binding,
                                 root_b / "node-arena-v2" / "source-binding-v2.bin"),
                 "replace with valid-CRC binding matching second store")) {
        return false;
    }

    LogicalNodeArenaV2StoreBoundReader reader(root_b);
    return require(!reader.open(&error),
                   "mixed valid arena/binding from different partitions is rejected") &&
        require(error.find("does not bind nested arena") != std::string::npos,
                "composite identity blocks valid-CRC partition Frankenstein");
}

} // namespace

int main() {
    if (!test_round_trip_and_v1_isolation() ||
        !test_create_only_publication() ||
        !test_binding_crc_tamper_fails_closed() ||
        !test_marker_crc_tamper_fails_closed() ||
        !test_valid_crc_partition_frankenstein_fails_closed()) {
        return 1;
    }
    std::cout << "Logical node arena v2 tests passed\n";
    return 0;
}
