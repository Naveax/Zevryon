#include "html_tokenizer_token_stream_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::massivedoc::HtmlTokenizerV1InitialState;
using zevryon::massivedoc::HtmlTokenizerV1ParseError;
using zevryon::massivedoc::HtmlTokenizerV1Sink;
using zevryon::massivedoc::HtmlTokenizerV1Stats;
using zevryon::massivedoc::HtmlTokenizerV1Token;
using zevryon::massivedoc::HtmlTokenizerV1TokenKind;
using zevryon::massivedoc::tokenize_html_token_stream_v1;

int hex_nibble(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool decode_hex(std::string_view encoded, std::string* output) {
    if ((encoded.size() % 2U) != 0U) {
        return false;
    }
    output->clear();
    output->reserve(encoded.size() / 2U);
    for (std::size_t index = 0U; index < encoded.size(); index += 2U) {
        const int high = hex_nibble(encoded[index]);
        const int low = hex_nibble(encoded[index + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        const unsigned int byte =
            (static_cast<unsigned int>(high) << 4U) |
            static_cast<unsigned int>(low);
        output->push_back(static_cast<char>(byte));
    }
    return true;
}

std::string encode_hex(std::string_view input) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2U);
    for (char character : input) {
        const auto byte = static_cast<unsigned char>(character);
        output.push_back(digits[(byte >> 4U) & 0x0fU]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

bool parse_state(std::string_view value, HtmlTokenizerV1InitialState* state) {
    if (value == "DATA") {
        *state = HtmlTokenizerV1InitialState::Data;
        return true;
    }
    if (value == "PLAINTEXT") {
        *state = HtmlTokenizerV1InitialState::Plaintext;
        return true;
    }
    if (value == "RCDATA") {
        *state = HtmlTokenizerV1InitialState::Rcdata;
        return true;
    }
    if (value == "RAWTEXT") {
        *state = HtmlTokenizerV1InitialState::Rawtext;
        return true;
    }
    if (value == "SCRIPT_DATA") {
        *state = HtmlTokenizerV1InitialState::ScriptData;
        return true;
    }
    return false;
}

class CollectingSink final : public HtmlTokenizerV1Sink {
public:
    bool on_token(const HtmlTokenizerV1Token& token, std::string*) override {
        tokens.push_back(token);
        return true;
    }

    bool on_parse_error(
        const HtmlTokenizerV1ParseError& parse_error,
        std::string*) override {
        errors.push_back(parse_error);
        return true;
    }

    std::vector<HtmlTokenizerV1Token> tokens;
    std::vector<HtmlTokenizerV1ParseError> errors;
};

void emit_token(const HtmlTokenizerV1Token& token) {
    switch (token.kind) {
    case HtmlTokenizerV1TokenKind::Character:
        // Legacy v1 runner wire record. Keep this byte-for-byte stable.
        std::cout << "TOKEN\tC\t" << encode_hex(token.data) << '\n';
        return;
    case HtmlTokenizerV1TokenKind::EndTag:
        // Legacy v1 runner wire record. Keep this byte-for-byte stable.
        std::cout << "TOKEN\tE\t" << encode_hex(token.name) << '\n';
        return;
    case HtmlTokenizerV1TokenKind::StartTag:
        std::cout << "TOKEN\tS\t" << encode_hex(token.name) << '\t'
                  << (token.self_closing ? 1 : 0) << '\t'
                  << token.attributes.size();
        for (const auto& attribute : token.attributes) {
            std::cout << '\t' << encode_hex(attribute.name)
                      << '\t' << encode_hex(attribute.value);
        }
        std::cout << '\n';
        return;
    case HtmlTokenizerV1TokenKind::Comment:
        std::cout << "TOKEN\tM\t" << encode_hex(token.data) << '\n';
        return;
    case HtmlTokenizerV1TokenKind::Doctype:
        std::cout << "TOKEN\tD\t" << encode_hex(token.name) << '\t'
                  << (token.has_public_identifier ? 1 : 0) << '\t'
                  << encode_hex(token.public_identifier) << '\t'
                  << (token.has_system_identifier ? 1 : 0) << '\t'
                  << encode_hex(token.system_identifier) << '\t'
                  << (token.force_quirks ? 1 : 0) << '\n';
        return;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: probe <DATA|PLAINTEXT|RCDATA|RAWTEXT|SCRIPT_DATA> <last-tag-hex> <input-hex>\n";
        return 64;
    }

    HtmlTokenizerV1InitialState state{HtmlTokenizerV1InitialState::Data};
    if (!parse_state(argv[1], &state)) {
        std::cerr << "invalid state\n";
        return 64;
    }

    std::string last_start_tag;
    std::string input;
    if (!decode_hex(argv[2], &last_start_tag) ||
        !decode_hex(argv[3], &input)) {
        std::cerr << "invalid hex input\n";
        return 64;
    }

    CollectingSink sink;
    HtmlTokenizerV1Stats stats;
    std::string error;
    if (!tokenize_html_token_stream_v1(
            input,
            state,
            last_start_tag,
            {},
            &sink,
            &stats,
            &error)) {
        std::cout << "FAIL\t" << encode_hex(error) << '\n';
        return 2;
    }

    for (const HtmlTokenizerV1Token& token : sink.tokens) {
        emit_token(token);
    }
    for (const HtmlTokenizerV1ParseError& parse_error : sink.errors) {
        std::cout << "ERROR\t" << encode_hex(parse_error.code) << '\t'
                  << parse_error.line << '\t' << parse_error.column << '\n';
    }
    std::cout << "STATS\t"
              << stats.input_bytes << '\t'
              << stats.tokens_emitted << '\t'
              << stats.character_tokens_emitted << '\t'
              << stats.character_bytes_emitted << '\t'
              << stats.end_tags_emitted << '\t'
              << stats.parse_errors_emitted << '\n';
    return 0;
}
