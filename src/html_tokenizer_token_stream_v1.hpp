#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::massivedoc {

enum class HtmlTokenizerV1InitialState : std::uint8_t {
    Plaintext,
    Rcdata,
    Rawtext,
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
// tokenizer-conformance surface. V1 accepts PLAINTEXT, RCDATA and RAWTEXT as
// explicit initial states. RCDATA/RAWTEXT use last_start_tag for appropriate
// end-tag matching and transition to the admitted Data-state end-tag subset
// after an appropriate close. Character output is coalesced before token
// boundaries and is bounded by maximum_token_bytes.
//
// This is intentionally not full WHATWG tokenization. Script-data, CDATA,
// complete named/numeric character references and input-stream preprocessing
// remain fail-closed. Data-state tags and markup-declaration admission live in
// their separately bounded production slices while sharing this event schema.
bool tokenize_html_token_stream_v1(
    std::string_view input,
    HtmlTokenizerV1InitialState initial_state,
    std::string_view last_start_tag,
    HtmlTokenizerV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerV1Stats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
