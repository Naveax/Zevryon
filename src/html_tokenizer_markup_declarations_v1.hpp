#pragma once

#include "html_tokenizer_token_stream_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace zevryon::massivedoc {

struct HtmlTokenizerMarkupDeclarationsV1Config {
    std::size_t maximum_token_bytes{64U * 1024U};
};

struct HtmlTokenizerMarkupDeclarationsV1Stats {
    std::uint64_t tokens_emitted{0U};
    std::uint64_t comment_tokens_emitted{0U};
    std::uint64_t doctype_tokens_emitted{0U};
    std::uint64_t parse_errors_emitted{0U};
};

// Consume exactly one Data-state markup declaration beginning at
// input[markup_open_offset] == '<' and input[markup_open_offset + 1] == '!'.
// On success next_offset is the first byte after the consumed declaration.
// Token and parse-error events are delivered directly through the shared
// production tokenizer sink. The caller must provide prefix/location context
// that has already passed the v1 preprocessing authority; declaration bytes
// themselves are validated here. The admitted surface covers bounded comments,
// bounded ASCII DOCTYPE names, PUBLIC/SYSTEM quoted identifiers, the associated
// ASCII malformed-DOCTYPE recovery states, and bogus-comment recovery for
// incorrectly opened declarations. Raw NUL replacement, non-ASCII DOCTYPE
// preprocessing/location authority, and nullable missing-name token emission
// remain fail closed.
bool consume_html_markup_declaration_v1(
    std::string_view input,
    std::size_t markup_open_offset,
    HtmlTokenizerMarkupDeclarationsV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerMarkupDeclarationsV1Stats* stats,
    std::size_t* next_offset,
    std::string* error);

} // namespace zevryon::massivedoc
