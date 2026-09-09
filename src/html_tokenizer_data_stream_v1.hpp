#pragma once

#include "html_tokenizer_token_stream_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace zevryon::massivedoc {

struct HtmlTokenizerDataStreamV1Config {
    std::size_t maximum_input_bytes{1024U * 1024U};
    std::size_t maximum_token_bytes{64U * 1024U};
    std::size_t maximum_attributes{256U};
};

struct HtmlTokenizerDataStreamV1Stats {
    std::uint64_t input_bytes{0U};
    std::uint64_t tokens_emitted{0U};
    std::uint64_t character_tokens_emitted{0U};
    std::uint64_t character_bytes_emitted{0U};
    std::uint64_t start_tags_emitted{0U};
    std::uint64_t end_tags_emitted{0U};
    std::uint64_t comment_tokens_emitted{0U};
    std::uint64_t doctype_tokens_emitted{0U};
    std::uint64_t attributes_emitted{0U};
    std::uint64_t parse_errors_emitted{0U};
    std::uint64_t data_segments_consumed{0U};
    std::uint64_t markup_declarations_consumed{0U};
};

// Compose the admitted Data-tag tokenizer and markup-declaration tokenizer into
// one bounded Data-state production stream. The coordinator preserves event
// order, translates delegated parse-error locations back to original-input
// coordinates and advances source position incrementally so declaration-heavy
// inputs do not repeatedly rescan already consumed prefixes. It only splits on
// <! while in Data state; declaration-looking bytes inside quoted/tag syntax
// stay with the tag parser.
//
// This is a streaming, non-transactional API: tokens or parse errors accepted
// by the sink before a later fail-closed condition remain published. The
// *_consumed counters advance only after the corresponding delegated unit
// completes successfully.
//
// Character references, NUL/preprocessing, non-ASCII authority, script-data,
// CDATA and recovery outside the admitted component surfaces remain fail
// closed. This API is not a full WHATWG tokenizer-conformance claim.
bool tokenize_html_data_stream_v1(
    std::string_view input,
    HtmlTokenizerDataStreamV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerDataStreamV1Stats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
