#include "logical_node_arena.hpp"
#include "logical_node_arena_v2.hpp"

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

using zevryon::massivedoc::LogicalNodeArenaBuildConfig;
using zevryon::massivedoc::LogicalNodeArenaReader;
using zevryon::massivedoc::LogicalNodeArenaV2Reader;
using zevryon::massivedoc::LogicalNodeArenaV2Writer;
using zevryon::massivedoc::LogicalNodeAttributeInput;
using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::LogicalNodeRecord;
using zevryon::massivedoc::LogicalSemanticKind;
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

LogicalNodeArenaBuildConfig config(std::uint8_t source_byte) {
    LogicalNodeArenaBuildConfig result;
    result.candidate_commit =
        "0123456789abcdef0123456789abcdef01234567";
    result.candidate_tree =
        "89abcdef0123456789abcdef0123456789abcdef";
    result.source_sha256.fill(source_byte);
    result.semantic_bucket_count = 64U;
    result.semantic_hash_bits = 64U;
    return result;
}

bool build_arena(
    const std::filesystem::path& root,
    std::uint8_t source_byte,
    std::string* error) {
    LogicalNodeArenaV2Writer writer(root, config(source_byte));
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
            error)) {
        return false;
    }
    if (!writer.append_node(
            LogicalNodeInput{
                2U,
                0U,
                1U,
                6U,
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

bool flip_marker_payload_byte(const std::filesystem::path& marker) {
    std::fstream stream(marker, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        return false;
    }
    constexpr std::streamoff kNodeCountOffset = 32;
    stream.seekg(kNodeCountOffset, std::ios::beg);
    char value = 0;
    stream.read(&value, 1);
    if (!stream) {
        return false;
    }
    value = static_cast<char>(static_cast<unsigned char>(value) ^ 0x01U);
    stream.clear();
    stream.seekp(kNodeCountOffset, std::ios::beg);
    stream.write(&value, 1);
    stream.flush();
    return static_cast<bool>(stream);
}

bool test_round_trip_and_v1_isolation() {
    const std::filesystem::path root = unique_root("node-arena-v2-roundtrip");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_arena(root, 0x11U, &error), error) ||
        !require(
            std::filesystem::is_directory(root / "node-arena-v2"),
            "v2 authoritative directory exists") ||
        !require(
            !std::filesystem::exists(root / "node-arena-v2.building"),
            "v2 staging directory is gone after publication") ||
        !require(
            !std::filesystem::exists(root / "node-arena"),
            "v2 publication does not create a v1 authoritative root")) {
        return false;
    }

    {
        LogicalNodeArenaReader old_reader(root);
        if (!require(
                !old_reader.open(&error),
                "v1 reader cannot silently open isolated v2 arena")) {
            return false;
        }
    }

    LogicalNodeRecord text;
    std::string semantic;
    {
        LogicalNodeArenaV2Reader reader(root);
        if (!require(reader.open(&error), error) ||
            !require(
                reader.manifest().format_version ==
                    kLogicalNodeArenaV2FormatVersion,
                "wrapper exposes v2 format") ||
            !require(
                reader.manifest().storage_manifest.node_count == 2U,
                "nested storage node count") ||
            !require(
                reader.manifest().storage_manifest.attribute_count == 1U,
                "nested storage attribute count") ||
            !require(reader.node_by_ordinal(1U, &text, &error), error) ||
            !require(
                text.source_record_index == 0U &&
                    text.source_byte_offset == 1U &&
                    text.source_byte_length == 6U,
                "cross-record span triple survives disk round trip") ||
            !require(
                reader.resolve_semantic(
                    LogicalSemanticKind::tag,
                    text.tag_id,
                    &semantic,
                    &error),
                error) ||
            !require(semantic == "#text", "text semantic survives v2 wrapper")) {
            return false;
        }
    }
    return true;
}

bool test_create_only_publication() {
    const std::filesystem::path root = unique_root("node-arena-v2-create-only");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_arena(root, 0x22U, &error), error)) {
        return false;
    }
    LogicalNodeArenaV2Writer second(root, config(0x22U));
    if (!require(
            !second.begin(&error),
            "second v2 writer cannot overwrite authoritative arena")) {
        return false;
    }
    return true;
}

bool test_marker_crc_tamper_fails_closed() {
    const std::filesystem::path root = unique_root("node-arena-v2-crc");
    RootCleanup cleanup(root);
    std::string error;
    if (!require(build_arena(root, 0x33U, &error), error) ||
        !require(
            flip_marker_payload_byte(
                root / "node-arena-v2" / "manifest-v2.bin"),
            "marker CRC tamper fixture")) {
        return false;
    }
    LogicalNodeArenaV2Reader reader(root);
    if (!require(!reader.open(&error), "marker with invalid CRC is rejected") ||
        !require(
            error.find("CRC") != std::string::npos,
            "marker payload tamper reaches CRC authority")) {
        return false;
    }
    return true;
}

bool test_valid_crc_marker_transplant_fails_closed() {
    const std::filesystem::path root_a = unique_root("node-arena-v2-transplant-a");
    const std::filesystem::path root_b = unique_root("node-arena-v2-transplant-b");
    RootCleanup cleanup_a(root_a);
    RootCleanup cleanup_b(root_b);
    std::string error;
    if (!require(build_arena(root_a, 0x44U, &error), error) ||
        !require(build_arena(root_b, 0x55U, &error), error) ||
        !require(
            copy_file_bytes(
                root_a / "node-arena-v2" / "manifest-v2.bin",
                root_b / "node-arena-v2" / "manifest-v2.bin"),
            "valid-CRC marker transplant fixture")) {
        return false;
    }

    LogicalNodeArenaV2Reader reader(root_b);
    if (!require(
            !reader.open(&error),
            "marker from another nested arena is rejected despite valid CRC") ||
        !require(
            error.find("does not bind") != std::string::npos,
            "marker transplant reaches nested-manifest binding authority")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_round_trip_and_v1_isolation() ||
        !test_create_only_publication() ||
        !test_marker_crc_tamper_fails_closed() ||
        !test_valid_crc_marker_transplant_fails_closed()) {
        return 1;
    }
    std::cout << "Logical node arena v2 tests passed\n";
    return 0;
}
