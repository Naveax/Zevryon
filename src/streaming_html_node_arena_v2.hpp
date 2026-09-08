#pragma once

#include "logical_node_v2_import.hpp"
#include "streaming_html_node_source_v2.hpp"

#include <filesystem>
#include <string>

namespace zevryon::massivedoc {

struct StreamingHtmlNodeArenaV2Config {
    StreamingHtmlNodeSourceConfig parser{};
    LogicalNodeV2ImportConfig import{};
};

struct StreamingHtmlNodeArenaV2Stats {
    StreamingHtmlNodeSourceV2Stats parser{};
    LogicalNodeV2ImportStats import{};
    bool source_published{false};
    bool arena_published{false};
};

// Transactional production bridge from an authoritative native store to the
// store-bound logical-node arena-v2 runtime representation.
//
// The caller supplies a create-only ZVNSRC01 v2 source path whose parent must
// resolve outside the authoritative native store tree. This prevents the
// orchestration artifact itself from mutating the store it is supposed to bind.
// On parser success that source is imported into store_root/node-arena-v2/.
//
// If import fails, only the source file published by this invocation is rolled
// back; a pre-existing caller file is never removed because parser publication
// must have succeeded before rollback is armed. Arena staging cleanup remains
// owned by the importer.
//
// On success both the authoritative source stream and the bound arena remain
// published so later validation can replay the exact parser-to-arena boundary.
bool build_streaming_html_node_arena_v2(
    const std::filesystem::path& store_root,
    const std::filesystem::path& source_path,
    StreamingHtmlNodeArenaV2Config config,
    StreamingHtmlNodeArenaV2Stats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
