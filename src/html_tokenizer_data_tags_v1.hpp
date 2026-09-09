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
    std::uint64_t attributes_emitted{0U};
    std::uint64_t parse_errors_emitted{0U};
};

// Bounded production Data-state tag-tokenization slice. The function emits
// HtmlTokenizerV1Token events directly through the shared tokenizer sink.
// It admits ordinary start/end tags, quoted/unquoted/empty attributes,
// duplicate-attribute handling, missing-whitespace diagnostics and the
// self-closing start-tag flag. Character data outside tags is coalesced.
//
// This slice intentionally does not approximate markup declarations,
// comments, DOCTYPE, character references, NUL/input preprocessing or broad
// malformed-tag recovery. Those surfaces fail closed until admitted by later
// conformance slices.
bool tokenize_html_data_tags_v1(
    std::string_view input,
    HtmlTokenizerDataTagsV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerDataTagsV1Stats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
