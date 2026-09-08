#include "logical_node_arena.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using zevryon::massivedoc::LogicalNodeArenaBuildConfig;
using zevryon::massivedoc::LogicalNodeArenaReader;
using zevryon::massivedoc::LogicalNodeArenaWriter;
using zevryon::massivedoc::LogicalNodeAttributeInput;
using zevryon::massivedoc::LogicalNodeAttributeRecord;
using zevryon::massivedoc::LogicalNodeInput;
using zevryon::massivedoc::LogicalNodeRecord;
using zevryon::massivedoc::LogicalSemanticKind;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: logical node arena: " << message << '\n';
        return false;
    }
    return true;
}

void cleanup_root(const std::filesystem::path& root) noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

struct RootGuard {
    std::filesystem::path root;

    ~RootGuard() {
        cleanup_root(root);
    }
};

std::filesystem::path unique_root(std::string_view name) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("zevryon-") + std::string(name) + "-" + std::to_string(tick));
}

LogicalNodeArenaBuildConfig config(std::uint32_t hash_bits = 0U, std::uint32_t buckets = 64U) {
    LogicalNodeArenaBuildConfig result;
    result.candidate_commit = "0123456789abcdef0123456789abcdef01234567";
    result.candidate_tree = "89abcdef0123456789abcdef0123456789abcdef";
    result.semantic_bucket_count = buckets;
    result.semantic_hash_bits = hash_bits;
    for (std::size_t index = 0U; index < result.source_sha256.size(); ++index) {
        result.source_sha256[index] = static_cast<std::uint8_t>(index);
    }
    return result;
}

bool append_fixture(LogicalNodeArenaWriter* writer, std::string* error) {
    const std::array<LogicalNodeAttributeInput, 2> root_attributes{{
        {"lang", "en", 0U},
        {"class", "page", 0U},
    }};
    if (!writer->append_node(
            LogicalNodeInput{1U, 0U, 0U, 100U, kNoLogicalNodeOrdinal, "html", "", "display:block", 1U},
            root_attributes,
            error)) {
        return false;
    }

    const std::array<LogicalNodeAttributeInput, 2> first_child_attributes{{
        {"class", "panel", 0U},
        {"data-empty", "", 7U},
    }};
    if (!writer->append_node(
            LogicalNodeInput{2U, 1U, 100U, 50U, 0U, "div", "main", "display:block", 2U},
            first_child_attributes,
            error)) {
        return false;
    }

    if (!writer->append_node(
            LogicalNodeInput{3U, 2U, 150U, 25U, 1U, "span", "text", "color:red", 3U},
            {},
            error)) {
        return false;
    }

    const std::array<LogicalNodeAttributeInput, 1> sibling_attributes{{
        {"class", "panel", 0U},
    }};
    return writer->append_node(
        LogicalNodeInput{4U, 3U, 175U, 25U, 0U, "div", "main", "display:block", 4U},
        sibling_attributes,
        error);
}

bool build_fixture(const std::filesystem::path& root, std::string* error) {
    LogicalNodeArenaWriter writer(root, config());
    return writer.begin(error) && append_fixture(&writer, error) && writer.finish(error);
}

bool test_round_trip_and_interning() {
    const std::filesystem::path root = unique_root("node-arena-roundtrip");
    const RootGuard guard{root};
    std::string error;
    LogicalNodeArenaWriter writer(root, config());
    if (!require(writer.begin(&error), error) ||
        !require(append_fixture(&writer, &error), error) ||
        !require(writer.node_count() == 4U, "writer node count") ||
        !require(writer.attribute_count() == 5U, "writer attribute count") ||
        !require(writer.semantic_bucket_head_bytes() == 5U * 64U * 8U, "bounded bucket-head bytes") ||
        !require(writer.finish(&error), error)) {
        return false;
    }

    LogicalNodeArenaWriter overwrite(root, config());
    if (!require(!overwrite.begin(&error), "create-only arena publication")) {
        return false;
    }

    LogicalNodeArenaReader reader(root);
    if (!require(reader.open(&error), error) ||
        !require(reader.manifest().node_count == 4U, "manifest node count") ||
        !require(reader.manifest().attribute_count == 5U, "manifest attribute count") ||
        !require(reader.manifest().candidate_commit == config().candidate_commit, "candidate commit binding") ||
        !require(reader.manifest().candidate_tree == config().candidate_tree, "candidate tree binding")) {
        return false;
    }

    LogicalNodeRecord root_node;
    LogicalNodeRecord first_child;
    LogicalNodeRecord grandchild;
    LogicalNodeRecord sibling;
    if (!require(reader.node_by_ordinal(0U, &root_node, &error), error) ||
        !require(reader.node_by_id(2U, &first_child, &error), error) ||
        !require(reader.node_by_ordinal(2U, &grandchild, &error), error) ||
        !require(reader.node_by_ordinal(3U, &sibling, &error), error) ||
        !require(root_node.first_child_ordinal == 1U, "root first child") ||
        !require(first_child.parent_ordinal == 0U, "first child parent") ||
        !require(first_child.first_child_ordinal == 2U, "nested first child") ||
        !require(first_child.next_sibling_ordinal == 3U, "root sibling chain") ||
        !require(grandchild.parent_ordinal == 1U, "grandchild parent") ||
        !require(sibling.parent_ordinal == 0U, "sibling parent") ||
        !require(first_child.tag_id == sibling.tag_id, "repeated tag intern id reuse") ||
        !require(first_child.role_id == sibling.role_id, "repeated role intern id reuse") ||
        !require(first_child.style_id == root_node.style_id, "repeated style intern id reuse") ||
        !require(first_child.style_id == sibling.style_id, "style reuse across siblings")) {
        return false;
    }

    std::string semantic;
    if (!require(reader.resolve_semantic(LogicalSemanticKind::tag, first_child.tag_id, &semantic, &error), error) ||
        !require(semantic == "div", "tag round trip") ||
        !require(reader.resolve_semantic(LogicalSemanticKind::style, first_child.style_id, &semantic, &error), error) ||
        !require(semantic == "display:block", "style round trip")) {
        return false;
    }

    LogicalNodeAttributeRecord first_attribute;
    LogicalNodeAttributeRecord empty_attribute;
    LogicalNodeAttributeRecord sibling_attribute;
    if (!require(reader.attribute_by_ordinal(first_child.attribute_offset, &first_attribute, &error), error) ||
        !require(reader.attribute_by_ordinal(first_child.attribute_offset + 1U, &empty_attribute, &error), error) ||
        !require(reader.attribute_by_ordinal(sibling.attribute_offset, &sibling_attribute, &error), error) ||
        !require(first_attribute.name_id == sibling_attribute.name_id, "attribute-name intern id reuse") ||
        !require(first_attribute.value_id == sibling_attribute.value_id, "attribute-value intern id reuse") ||
        !require(empty_attribute.value_id != 0U, "empty attribute value has real intern id") ||
        !require(reader.resolve_semantic(
            LogicalSemanticKind::attribute_value, empty_attribute.value_id, &semantic, &error), error) ||
        !require(semantic.empty(), "empty attribute value round trip") ||
        !require(!reader.attribute_by_ordinal(reader.manifest().attribute_count, &empty_attribute, &error),
                 "attribute upper bound fails closed")) {
        return false;
    }
    return true;
}

bool test_multiple_roots_rejected() {
    const std::filesystem::path root = unique_root("node-arena-multiple-root");
    const RootGuard guard{root};
    std::string error;
    {
        LogicalNodeArenaWriter writer(root, config());
        if (!require(writer.begin(&error), error) ||
            !require(writer.append_node(
                LogicalNodeInput{1U, 0U, 0U, 1U, kNoLogicalNodeOrdinal, "root", "", "", 0U},
                {},
                &error), error) ||
            !require(!writer.append_node(
                LogicalNodeInput{2U, 1U, 1U, 1U, kNoLogicalNodeOrdinal, "root", "", "", 0U},
                {},
                &error),
                "second root rejected")) {
            return false;
        }
    }
    return require(!std::filesystem::exists(root / "node-arena"),
                   "failed writer cannot publish authoritative arena");
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

bool copy_arena(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::create_directories(target, error);
    if (error) {
        return false;
    }
    std::filesystem::copy(
        source / "node-arena",
        target / "node-arena",
        std::filesystem::copy_options::recursive,
        error);
    return !error;
}

bool test_corruption_fails_closed() {
    const std::filesystem::path original = unique_root("node-arena-corrupt-source");
    const RootGuard original_guard{original};
    std::string error;
    if (!require(build_fixture(original, &error), error)) {
        return false;
    }

    const std::filesystem::path manifest_root = unique_root("node-arena-manifest-tamper");
    const RootGuard manifest_guard{manifest_root};
    if (!require(copy_arena(original, manifest_root), "copy manifest fixture") ||
        !require(flip_byte(manifest_root / "node-arena" / "manifest.bin", 24U), "tamper manifest")) {
        return false;
    }
    LogicalNodeArenaReader manifest_reader(manifest_root);
    if (!require(!manifest_reader.open(&error), "manifest CRC tamper rejected")) {
        return false;
    }

    const std::filesystem::path index_root = unique_root("node-arena-index-truncate");
    const RootGuard index_guard{index_root};
    if (!require(copy_arena(original, index_root), "copy index fixture")) {
        return false;
    }
    std::error_code fs_error;
    std::filesystem::resize_file(index_root / "node-arena" / "tag.offsets", 0U, fs_error);
    if (!require(!fs_error, "truncate tag offsets")) {
        return false;
    }
    LogicalNodeArenaReader index_reader(index_root);
    if (!require(!index_reader.open(&error), "dictionary index truncation rejected")) {
        return false;
    }

    const std::filesystem::path dictionary_root = unique_root("node-arena-dictionary-tamper");
    const RootGuard dictionary_guard{dictionary_root};
    if (!require(copy_arena(original, dictionary_root), "copy dictionary fixture") ||
        !require(flip_byte(dictionary_root / "node-arena" / "tag.entries", 32U), "tamper tag payload")) {
        return false;
    }
    LogicalNodeArenaReader dictionary_reader(dictionary_root);
    LogicalNodeRecord node;
    std::string semantic;
    if (!require(dictionary_reader.open(&error), error) ||
        !require(dictionary_reader.node_by_ordinal(0U, &node, &error), error) ||
        !require(!dictionary_reader.resolve_semantic(LogicalSemanticKind::tag, node.tag_id, &semantic, &error),
                 "dictionary CRC tamper rejected")) {
        return false;
    }
    return true;
}

bool test_large_shallow_stream_is_bounded() {
    const std::filesystem::path root = unique_root("node-arena-large");
    const RootGuard guard{root};
    std::string error;
    LogicalNodeArenaWriter writer(root, config(64U, 128U));
    if (!require(writer.begin(&error), error) ||
        !require(writer.append_node(
            LogicalNodeInput{1U, 0U, 0U, 1U, kNoLogicalNodeOrdinal, "root", "", "", 0U},
            {},
            &error), error)) {
        return false;
    }
    constexpr std::uint64_t kChildren = 20000U;
    for (std::uint64_t index = 0U; index < kChildren; ++index) {
        if (!require(writer.append_node(
                LogicalNodeInput{
                    index + 2U,
                    index + 1U,
                    index + 1U,
                    1U,
                    0U,
                    "item",
                    "",
                    "",
                    0U},
                {},
                &error), error)) {
            return false;
        }
    }
    if (!require(writer.semantic_bucket_head_bytes() == 5U * 128U * 8U,
                 "semantic RAM index stays fixed across 20k nodes") ||
        !require(writer.finish(&error), error)) {
        return false;
    }
    LogicalNodeArenaReader reader(root);
    LogicalNodeRecord tail;
    if (!require(reader.open(&error), error) ||
        !require(reader.node_by_id(kChildren + 1U, &tail, &error), error) ||
        !require(tail.parent_ordinal == 0U, "large shallow tail parent") ||
        !require(tail.logical_id == kChildren + 1U, "large shallow tail identity")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_round_trip_and_interning() ||
        !test_multiple_roots_rejected() ||
        !test_corruption_fails_closed() ||
        !test_large_shallow_stream_is_bounded()) {
        return 1;
    }
    return 0;
}
