#pragma once

#include "html_tokenizer_token_stream_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace zevryon::massivedoc {

struct HtmlTokenizerDataTagsV1Config {
    std::size_t maximum_input_bytes{1024U * 1024U};
    std::size_t maximum_token_bytes{64U * 1024U};
    std::size_t maximum_attributes{256U};
};

struct HtmlTokenizerDataTagsV1Stats {
    std::uint64_t input_bytes{0U};
    std::uint64_t tokens_emitted{0U};
    std::uint64_t character_tokens_emitted{0U};
    std::uint64_t character_bytes_emitted{0U};
    std::uint64_t start_tags_emitted{0U};
    std::uint64_t end_tags_emitted{0U};
    std::uint64_t comment_tokens_emitted{0U};
    std::uint64_t attributes_emitted{0U};
    std::uint64_t parse_errors_emitted{0U};
};

// Bounded production Data-state tag-tokenization slice. The function emits
// HtmlTokenizerV1Token events directly through the shared tokenizer sink.
// It admits ordinary start/end tags, quoted/unquoted/empty attributes,
// duplicate-attribute handling, missing-whitespace diagnostics, the
// self-closing start-tag flag, tag-open invalid-ASCII reconsume, empty-end-tag
// recovery and the five parse-error special bytes in unquoted attribute
// values. Character data outside tags is coalesced.
//
// Character-reference admission includes literal ampersand fallback,
// bounded decimal/hex numeric references and the complete pinned WHATWG named
// reference table in Data and attribute-value contexts. Decoded references may
// produce UTF-8 output bytes. Raw Data character text additionally admits
// well-formed UTF-8 Unicode scalars and preserves their exact UTF-8 bytes;
// malformed UTF-8 fails closed. Parse-error columns across admitted UTF-8 Data
// text count Unicode scalars rather than encoding bytes.
//
// This slice intentionally does not approximate markup declarations,
// comments, DOCTYPE, NUL/input preprocessing, CR/LF preprocessing, non-ASCII
// tag/attribute names or raw non-ASCII attribute values, nor the remaining malformed-
// tag recovery states. Those surfaces fail closed until
// separately admitted by later conformance slices.
bool tokenize_html_data_tags_v1(
    std::string_view input,
    HtmlTokenizerDataTagsV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerDataTagsV1Stats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
