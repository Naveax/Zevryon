#include "logical_node_v2_import.hpp"

#include "logical_node_arena_v2_store_bound.hpp"
#include "logical_node_source.hpp"
#include "massivedoc_store.hpp"

#include <limits>
#include <span>
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

bool same_binding(
    const LogicalNodeSourceStoreBinding& left,
    const LogicalNodeSourceStoreBinding& right) noexcept {
    return left.source_record_count == right.source_record_count &&
        left.payload_sha256 == right.payload_sha256 &&
        left.record_sequence_sha256 == right.record_sequence_sha256;
}

bool validate_node_span(
    const StoreReader& store,
    const LogicalNodeSourceNode& node,
    std::uint64_t* delivered,
    std::string* error) {
    if (delivered == nullptr || error == nullptr) {
        return false;
    }
    *delivered = 0U;
    std::string span_error;
    if (!store.read_record_span(
            node.source_record_index,
            node.source_byte_offset,
            node.source_byte_length,
            [&](std::span<const std::byte> bytes) {
                const std::uint64_t amount =
                    static_cast<std::uint64_t>(bytes.size());
                if (*delivered >
                    std::numeric_limits<std::uint64_t>::max() - amount) {
                    span_error = "logical node v2 import span byte counter overflows";
                    return false;
                }
                *delivered += amount;
                return true;
            },
            &span_error)) {
        return fail_import(
            error,
            "logical node v2 import span escapes authoritative store: " + span_error);
    }
    if (!span_error.empty()) {
        return fail_import(error, span_error);
    }
    if (*delivered != node.source_byte_length) {
        return fail_import(
            error,
            "logical node v2 import span delivery length disagrees with node metadata");
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

    LogicalNodeSourceStoreBinding actual_binding;
    if (!inspect_logical_node_source_store_binding(
            store_root, &actual_binding, error)) {
        return false;
    }

    // Keep one source reader open from first untrusted frame through final
    // replay. There is no validate-by-path / reopen-by-path TOCTOU boundary.
    LogicalNodeSourceV2Reader source(source_path);
    if (!source.open(error)) {
        return false;
    }
    if (!same_binding(source.manifest().store_binding, actual_binding)) {
        return fail_import(
            error,
            "logical node v2 source binding does not match authoritative store");
    }

    StoreReader store(store_root);
    if (!store.open(error)) {
        return false;
    }
    if (store.stats().corpus.logical_records != actual_binding.source_record_count ||
        source.manifest().node_count != store.stats().corpus.logical_nodes) {
        return fail_import(
            error,
            "logical node v2 source/store manifest counts disagree");
    }

    LogicalNodeArenaBuildConfig arena_config;
    arena_config.candidate_commit = std::move(config.candidate_commit);
    arena_config.candidate_tree = std::move(config.candidate_tree);
    arena_config.source_sha256 = source.manifest().store_binding.payload_sha256;
    arena_config.semantic_bucket_count = config.semantic_bucket_count;
    arena_config.semantic_hash_bits = config.semantic_hash_bits;

    // The bound writer independently re-inspects the native store. Require the
    // full physical binding to remain identical before any node is appended.
    LogicalNodeArenaV2StoreBoundWriter arena(store_root, std::move(arena_config));
    if (!arena.begin(error)) {
        return false;
    }
    if (!same_binding(arena.source_binding(), source.manifest().store_binding)) {
        return fail_import(
            error,
            "logical node v2 store changed between source binding and arena staging");
    }

    LogicalNodeV2ImportStats local_stats;
    for (;;) {
        LogicalNodeSourceNode node;
        bool has_node = false;
        if (!source.next(&node, &has_node, error)) {
            return false;
        }
        if (!has_node) {
            break;
        }

        // Validate this exact decoded node against the authoritative store
        // before copying any of its semantics into the arena staging tree.
        std::uint64_t delivered = 0U;
        if (!validate_node_span(store, node, &delivered, error)) {
            return false;
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
        ++local_stats.validation.nodes_validated;

        const std::uint64_t node_attribute_count =
            static_cast<std::uint64_t>(node.attributes.size());
        if (local_stats.attributes_imported >
                std::numeric_limits<std::uint64_t>::max() - node_attribute_count ||
            local_stats.validation.attributes_validated >
                std::numeric_limits<std::uint64_t>::max() - node_attribute_count) {
            return fail_import(error, "logical node v2 imported attribute count overflows");
        }
        local_stats.attributes_imported += node_attribute_count;
        local_stats.validation.attributes_validated += node_attribute_count;

        if (local_stats.validation.source_span_bytes_streamed >
            std::numeric_limits<std::uint64_t>::max() - delivered) {
            return fail_import(error, "logical node v2 streamed-byte counter overflows");
        }
        local_stats.validation.source_span_bytes_streamed += delivered;
        if (node.source_byte_length == 0U) {
            if (local_stats.validation.zero_length_spans ==
                std::numeric_limits<std::uint64_t>::max()) {
                return fail_import(error, "logical node v2 zero-length span counter overflows");
            }
            ++local_stats.validation.zero_length_spans;
        }
    }

    if (local_stats.nodes_imported != source.manifest().node_count ||
        local_stats.attributes_imported != source.manifest().attribute_count ||
        local_stats.nodes_imported != local_stats.validation.nodes_validated ||
        local_stats.attributes_imported != local_stats.validation.attributes_validated) {
        return fail_import(
            error,
            "logical node v2 import totals disagree with source manifest");
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
