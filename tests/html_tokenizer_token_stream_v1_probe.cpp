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
    return false;
}

class CollectingSink final : public HtmlTokenizerV1Sink {
public:
    bool on_token(const HtmlTokenizerV1Token& token, std::string* error) override {
        if (token.kind != HtmlTokenizerV1TokenKind::Character &&
            token.kind != HtmlTokenizerV1TokenKind::EndTag) {
            if (error != nullptr) {
                *error = "probe received token kind outside v1 runner protocol";
            }
            return false;
        }
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

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: probe <PLAINTEXT|RCDATA|RAWTEXT> <last-tag-hex> <input-hex>\n";
        return 64;
    }

    HtmlTokenizerV1InitialState state{HtmlTokenizerV1InitialState::Plaintext};
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
        if (token.kind == HtmlTokenizerV1TokenKind::Character) {
            std::cout << "TOKEN\tC\t" << encode_hex(token.data) << '\n';
        } else {
            std::cout << "TOKEN\tE\t" << encode_hex(token.name) << '\n';
        }
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
