#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace zevryon::massivedoc {

// First production parser slice. This profile intentionally implements a
// strict, fail-closed HTML element-tree subset rather than pretending to be a
// complete WHATWG tree builder. Unsupported raw-text/foreign-content semantics
// are rejected until their tokenizer/tree-builder states are implemented.
struct StreamingHtmlNodeSourceConfig {
    std::size_t input_window_bytes{64U * 1024U};
    std::size_t maximum_token_bytes{1024U * 1024U};
    std::uint32_t maximum_attributes_per_element{4096U};
    std::uint32_t maximum_open_element_depth{4096U};

    // Hard cap for parser-owned resident allocations. Parser strings/vectors
    // are allocated through LedgerMemoryResource and charged before allocation.
    // StoreReader's source window remains governed by its own SourceWindow path.
    std::size_t working_set_limit_bytes{16U * 1024U * 1024U};
};

struct StreamingHtmlNodeSourceStats {
    std::uint64_t source_records{0U};
    std::uint64_t source_bytes{0U};
    std::uint64_t nodes_emitted{0U};
    std::uint64_t attributes_emitted{0U};
    std::uint64_t comments_skipped{0U};
    std::uint64_t doctypes_skipped{0U};
    std::uint64_t cross_record_token_anchors{0U};
    std::uint32_t maximum_observed_open_depth{0U};

    std::size_t working_set_hard_limit_bytes{0U};
    std::size_t working_set_current_bytes{0U};
    std::size_t working_set_peak_bytes{0U};
    std::uint64_t working_set_reservations{0U};
    std::uint64_t working_set_releases{0U};
    std::uint64_t working_set_rejected_reservations{0U};
    std::uint64_t working_set_accounting_errors{0U};
};

// Streams the authoritative native StoreReader payload and emits an explicit
// ZVNSRC01 semantic node source. The output is create-only. The parser emits a
// real #document node plus element nodes, attributes, role and style semantics.
// Text-node materialization and the full WHATWG error-recovery/tree-builder
// state machine remain later Z7/Z8 slices and are not synthesized here.
//
// Parser-owned dynamic allocations are routed through a private Z0
// ResourceLedger/LedgerMemoryResource authority. A hard-cap rejection fails
// closed and cannot publish a partial source stream. Stats are returned on both
// success and parse/allocation failure when a non-null stats pointer is given.
//
// Fail-closed strict-subset rules include:
// - exact nesting for non-void elements;
// - bounded tag/comment tokens, attributes and open-element depth;
// - bounded parser working-set allocations;
// - no script/style/title/textarea/raw-text elements yet;
// - no foreign-content namespace semantics yet;
// - source node count must equal the store's logical_nodes envelope metadata.
bool produce_streaming_html_node_source(
    const std::filesystem::path& store_root,
    const std::filesystem::path& output_source_path,
    StreamingHtmlNodeSourceConfig config,
    StreamingHtmlNodeSourceStats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
