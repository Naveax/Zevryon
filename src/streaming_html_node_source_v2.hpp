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
// element/attribute subset while emitting normal data-state text nodes, the
// bounded RAWTEXT family (<style>, <xmp>, <iframe>, <noembed>, <noframes>),
// structural RCDATA node/source-span handling for <title> and <textarea>, and
// PLAINTEXT source-span handling from <plaintext> through EOF.
// Text payload bytes are not retained in parser memory: each text node stores
// only its authoritative cross-record raw source span.
//
// RCDATA character-reference decoding is not stored in this source format yet;
// this slice establishes browser text-node boundaries and authoritative source
// identity. A literal LF immediately after <textarea> is omitted from the text
// node as required by HTML parsing. CR/CRLF preprocessing remains fail-closed.
// PLAINTEXT treats every remaining non-NUL byte, including '<', '&' and apparent
// end tags, as text until EOF. NUL remains fail-closed because the v2 source
// stream does not yet carry the decoded U+FFFD replacement payload.
//
// This is still intentionally not a complete WHATWG tokenizer/tree builder.
// Script data, scripting-mode-dependent noscript behavior, complete input
// preprocessing/decoded text semantics, foreign-content roots and broader
// parse-error recovery remain fail-closed until implemented explicitly.
bool produce_streaming_html_node_source_v2(
    const std::filesystem::path& store_root,
    const std::filesystem::path& output_source_path,
    StreamingHtmlNodeSourceConfig config,
    StreamingHtmlNodeSourceV2Stats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
