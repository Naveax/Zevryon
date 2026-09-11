#pragma once

#include "html_tokenizer_token_stream_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace zevryon::massivedoc {

struct HtmlTokenizerScriptDataV1Stats {
    std::uint64_t input_bytes_available{0U};
    std::uint64_t bytes_consumed{0U};
    std::uint64_t tokens_emitted{0U};
    std::uint64_t character_tokens_emitted{0U};
    std::uint64_t character_bytes_emitted{0U};
    std::uint64_t end_tags_emitted{0U};
    std::uint64_t parse_errors_emitted{0U};
};

struct HtmlTokenizerScriptDataV1Result {
    std::size_t next_offset{0U};
    bool transitioned_to_data{false};
};

// Consume a bounded Script-data-state stream from input[0]. On ordinary EOF,
// next_offset == input.size() and transitioned_to_data is false. When an
// appropriate end tag is emitted, the function stops immediately after that
// tag and returns transitioned_to_data=true so a caller can explicitly compose
// the remaining suffix with the Data-state authority.
//
// The component implements the admitted ASCII Script-data escaped and
// double-escaped state family through the shared HtmlTokenizerV1Sink event
// boundary. Script-data U+0000 emits unexpected-null-character and U+FFFD in
// the states that consume Character data. Broader preprocessing and unsupported
// appropriate end-tag attribute/self-closing recovery remain fail closed.
bool consume_html_script_data_v1(
    std::string_view input,
    std::string_view last_start_tag,
    HtmlTokenizerV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerScriptDataV1Stats* stats,
    HtmlTokenizerScriptDataV1Result* result,
    std::string* error);

} // namespace zevryon::massivedoc
