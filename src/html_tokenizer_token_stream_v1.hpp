#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::massivedoc {

enum class HtmlTokenizerV1InitialState : std::uint8_t {
    Data,
    Plaintext,
    Rcdata,
    Rawtext,
    ScriptData,
};

enum class HtmlTokenizerV1TokenKind : std::uint8_t {
    Doctype,
    StartTag,
    EndTag,
    Comment,
    Character,
};

struct HtmlTokenizerV1Attribute {
    std::string name;
    std::string value;
};

struct HtmlTokenizerV1Token {
    HtmlTokenizerV1TokenKind kind{HtmlTokenizerV1TokenKind::Character};
    std::string name;
    std::vector<HtmlTokenizerV1Attribute> attributes;
    std::string data;
    bool self_closing{false};

    // DOCTYPE-only fields. Presence is explicit because the external tokenizer
    // authority distinguishes a missing identifier (null) from an empty one.
    std::string public_identifier;
    std::string system_identifier;
    bool has_public_identifier{false};
    bool has_system_identifier{false};
    bool force_quirks{false};
};

struct HtmlTokenizerV1ParseError {
    std::string code;
    std::uint64_t line{1U};
    std::uint64_t column{1U};
};

struct HtmlTokenizerV1Config {
    std::size_t maximum_input_bytes{1024U * 1024U};
    std::size_t maximum_token_bytes{64U * 1024U};
};

struct HtmlTokenizerV1Stats {
    std::uint64_t input_bytes{0U};
    std::uint64_t tokens_emitted{0U};
    std::uint64_t character_tokens_emitted{0U};
    std::uint64_t character_bytes_emitted{0U};
    std::uint64_t end_tags_emitted{0U};
    std::uint64_t parse_errors_emitted{0U};
};

class HtmlTokenizerV1Sink {
public:
    virtual ~HtmlTokenizerV1Sink() = default;

    virtual bool on_token(const HtmlTokenizerV1Token& token, std::string* error) = 0;
    virtual bool on_parse_error(
        const HtmlTokenizerV1ParseError& parse_error,
        std::string* error) = 0;
};

// Bounded production token-event boundary for the currently admitted external
// tokenizer-conformance surface. V1 accepts Data, PLAINTEXT, RCDATA, RAWTEXT
// and Script data as explicit initial states. Data delegates to the admitted
// bounded Data-stream coordinator, including literal ampersand fallback,
// bounded decimal/hex numeric references and the complete pinned WHATWG named
// character-reference table in Data/attribute contexts.
// RCDATA/RAWTEXT use last_start_tag for appropriate end-tag matching. Script
// data delegates to the admitted bounded Script-data state machine and, after
// an appropriate close, continues through the same admitted Data-stream
// composition on the unconsumed suffix. Character output is bounded by
// maximum_token_bytes.
//
// This remains intentionally narrower than complete WHATWG tokenization.
// CDATA, full input-stream preprocessing/non-ASCII raw-input location authority
// and broader recovery remain fail-closed where the admitted component surfaces
// do not yet implement them.
bool tokenize_html_token_stream_v1(
    std::string_view input,
    HtmlTokenizerV1InitialState initial_state,
    std::string_view last_start_tag,
    HtmlTokenizerV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerV1Stats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
