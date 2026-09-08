#pragma once

#include "logical_node_source_v2.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace zevryon::massivedoc {

struct LogicalNodeV2ImportConfig {
    std::string candidate_commit;
    std::string candidate_tree;
    std::uint32_t semantic_bucket_count{4096U};
    std::uint32_t semantic_hash_bits{64U};
};

struct LogicalNodeV2ImportStats {
    LogicalNodeSourceV2ValidationStats validation{};
    std::uint64_t nodes_imported{0U};
    std::uint64_t attributes_imported{0U};
};

// Imports a fully validated ZVNSRC01 v2 source into the authoritative,
// physical-store-bound ZVNODA v2 arena. Validation completes before arena
// staging begins, so an invalid source cannot create node-arena-v2.building/
// or publish node-arena-v2/. The destination writer independently derives and
// freezes the exact native-store payload + physical record-sequence identity.
bool import_logical_node_source_v2_to_arena_v2(
    const std::filesystem::path& source_path,
    const std::filesystem::path& store_root,
    LogicalNodeV2ImportConfig config,
    LogicalNodeV2ImportStats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
