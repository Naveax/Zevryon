#pragma once

#include "streaming_html_node_source.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace zevryon::massivedoc {

struct StreamingHtmlNodeSourceV2Stats {
    std::uint64_t source_records{0U};
    std::uint64_t source_bytes{0U};
    std::uint64_t nodes_emitted{0U};
    std::uint64_t element_nodes_emitted{0U};
    std::uint64_t text_nodes_emitted{0U};
    std::uint64_t attributes_emitted{0U};
    std::uint64_t comments_skipped{0U};
    std::uint64_t doctypes_skipped{0U};
    std::uint64_t cross_record_markup_spans{0U};
    std::uint64_t cross_record_text_spans{0U};
    std::uint32_t maximum_observed_open_depth{0U};

    std::uint64_t working_set_hard_limit_bytes{0U};
    std::uint64_t working_set_current_bytes{0U};
    std::uint64_t working_set_peak_bytes{0U};
    std::uint64_t working_set_reservations{0U};
    std::uint64_t working_set_releases{0U};
    std::uint64_t working_set_rejected_reservations{0U};
    std::uint64_t working_set_accounting_errors{0U};
};

// Strict streaming HTML producer for ZVNSRC01 v2. It preserves the admitted
// element/attribute subset while emitting normal data-state text nodes and the
// bounded RAWTEXT family currently admitted for <style>, <xmp>, <iframe>,
// <noembed> and <noframes>. Text payload bytes are not retained in parser
// memory: each text node stores only its authoritative cross-record source span.
//
// This is still intentionally not a complete WHATWG tokenizer/tree builder.
// Script data, RCDATA, plaintext, scripting-mode-dependent noscript behavior,
// foreign-content roots and broader parse-error recovery remain fail-closed
// until their states are implemented explicitly.
bool produce_streaming_html_node_source_v2(
    const std::filesystem::path& store_root,
    const std::filesystem::path& output_source_path,
    StreamingHtmlNodeSourceConfig config,
    StreamingHtmlNodeSourceV2Stats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
