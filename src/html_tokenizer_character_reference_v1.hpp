#pragma once

#include "html_tokenizer_token_stream_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace zevryon::massivedoc {

enum class HtmlTokenizerCharacterReferenceV1Context : std::uint8_t {
    Data,
    Attribute,
};

struct HtmlTokenizerCharacterReferenceV1Stats {
    std::uint64_t parse_errors_emitted{0U};
};

struct HtmlTokenizerCharacterReferenceV1Result {
    // First byte not consumed by this character-reference invocation.
    std::size_t next_offset{0U};

    // UTF-8 bytes to flush into the return state. For literal fallback this
    // contains the consumed temporary-buffer spelling such as "&" or "&#".
    std::string replacement_utf8;
};

// Consume one bounded character-reference state invocation beginning exactly
// at input[ampersand_offset] == '&'. V1 admits:
//   * literal '&' fallback when the next byte is neither ASCII alphanumeric
//     nor '#', including EOF;
//   * decimal and hexadecimal numeric character references;
//   * the complete pinned WHATWG named-character-reference table with bounded
//     maximum-length matching, one/two-scalar UTF-8 replacement, legacy
//     semicolon recovery and attribute-context historical veto;
//   * ambiguous-ampersand literal recovery and unknown-name diagnostics;
//   * the WHATWG numeric-reference end-state scalar/control/noncharacter rules.
//
// The context is explicit because semicolonless legacy named-reference behavior
// differs inside attributes. Raw non-ASCII input preprocessing remains outside
// this v1 character-reference component's authority.
//
// Parse errors are emitted through sink with one-based line/column positions
// relative to the supplied input view. No Character/attribute mutation is
// performed here; the caller flushes replacement_utf8 into its return state.
bool consume_html_character_reference_v1(
    std::string_view input,
    std::size_t ampersand_offset,
    HtmlTokenizerCharacterReferenceV1Context context,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerCharacterReferenceV1Stats* stats,
    HtmlTokenizerCharacterReferenceV1Result* result,
    std::string* error);

} // namespace zevryon::massivedoc
