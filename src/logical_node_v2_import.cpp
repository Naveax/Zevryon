#include "logical_node_v2_import.hpp"

#include "logical_node_arena_v2.hpp"

#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

bool fail_import(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool valid_candidate_hex(std::string_view value) noexcept {
    if (value.size() != 40U) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f') ||
              (character >= 'A' && character <= 'F'))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool import_logical_node_source_v2_to_arena_v2(
    const std::filesystem::path& source_path,
    const std::filesystem::path& store_root,
    LogicalNodeV2ImportConfig config,
    LogicalNodeV2ImportStats* stats,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (!valid_candidate_hex(config.candidate_commit) ||
        !valid_candidate_hex(config.candidate_tree) ||
        config.semantic_bucket_count == 0U ||
        config.semantic_hash_bits > 64U) {
        return fail_import(error, "logical node v2 import configuration is invalid");
    }

    // Complete the full untrusted-source/store validation pass before creating
    // any arena-v2 staging directory.
    LogicalNodeSourceV2ValidationStats validation;
    if (!validate_logical_node_source_v2_against_store(
            source_path,
            store_root,
            &validation,
            error)) {
        return false;
    }

    LogicalNodeSourceV2Reader source(source_path);
    if (!source.open(error)) {
        return false;
    }

    LogicalNodeArenaBuildConfig arena_config;
    arena_config.candidate_commit = std::move(config.candidate_commit);
    arena_config.candidate_tree = std::move(config.candidate_tree);
    arena_config.source_sha256 = source.manifest().store_binding.payload_sha256;
    arena_config.semantic_bucket_count = config.semantic_bucket_count;
    arena_config.semantic_hash_bits = config.semantic_hash_bits;

    LogicalNodeArenaV2Writer arena(store_root, std::move(arena_config));
    if (!arena.begin(error)) {
        return false;
    }

    LogicalNodeV2ImportStats local_stats;
    local_stats.validation = validation;
    for (;;) {
        LogicalNodeSourceNode node;
        bool has_node = false;
        if (!source.next(&node, &has_node, error)) {
            return false;
        }
        if (!has_node) {
            break;
        }

        std::vector<LogicalNodeAttributeInput> attributes;
        attributes.reserve(node.attributes.size());
        for (const LogicalNodeSourceAttribute& attribute : node.attributes) {
            attributes.push_back(LogicalNodeAttributeInput{
                attribute.name,
                attribute.value,
                attribute.flags});
        }

        if (!arena.append_node(
                LogicalNodeInput{
                    node.logical_id,
                    node.source_record_index,
                    node.source_byte_offset,
                    node.source_byte_length,
                    node.parent_ordinal,
                    node.tag,
                    node.role,
                    node.style,
                    node.flags},
                attributes,
                error)) {
            return false;
        }

        if (local_stats.nodes_imported ==
            std::numeric_limits<std::uint64_t>::max()) {
            return fail_import(error, "logical node v2 imported node count overflows");
        }
        ++local_stats.nodes_imported;
        if (local_stats.attributes_imported >
            std::numeric_limits<std::uint64_t>::max() - node.attributes.size()) {
            return fail_import(error, "logical node v2 imported attribute count overflows");
        }
        local_stats.attributes_imported +=
            static_cast<std::uint64_t>(node.attributes.size());
    }

    if (local_stats.nodes_imported != source.manifest().node_count ||
        local_stats.attributes_imported != source.manifest().attribute_count ||
        local_stats.nodes_imported != validation.nodes_validated ||
        local_stats.attributes_imported != validation.attributes_validated) {
        return fail_import(
            error,
            "logical node v2 import totals disagree with validated source manifest");
    }
    if (!arena.finish(error)) {
        return false;
    }
    if (stats != nullptr) {
        *stats = local_stats;
    }
    return true;
}

} // namespace zevryon::massivedoc
