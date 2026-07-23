#include "unicode_bidi_brackets.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string trim(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

bool parse_hex(const std::string& value, std::uint32_t* output) {
    try {
        std::size_t consumed = 0U;
        const unsigned long parsed = std::stoul(value, &consumed, 16);
        if (consumed != value.size() || parsed > 0x10FFFFUL) {
            return false;
        }
        *output = static_cast<std::uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: zevryon-bidi-bracket-conformance BidiBrackets.txt\n";
        return 2;
    }
    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "unable to open BidiBrackets data\n";
        return 2;
    }

    std::vector<bool> listed(0x110000U, false);
    std::string line;
    std::size_t line_number = 0U;
    std::uint64_t records = 0U;
    std::uint64_t opens = 0U;
    std::uint64_t closes = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        line = line.substr(0U, line.find('#'));
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        const std::size_t first_separator = line.find(';');
        const std::size_t second_separator = line.find(';', first_separator + 1U);
        if (first_separator == std::string::npos ||
            second_separator == std::string::npos ||
            line.find(';', second_separator + 1U) != std::string::npos) {
            std::cerr << "invalid line " << line_number << '\n';
            return 1;
        }
        std::uint32_t codepoint = 0U;
        std::uint32_t paired = 0U;
        if (!parse_hex(trim(line.substr(0U, first_separator)), &codepoint) ||
            !parse_hex(
                trim(line.substr(
                    first_separator + 1U,
                    second_separator - first_separator - 1U)),
                &paired)) {
            std::cerr << "invalid codepoint on line " << line_number << '\n';
            return 1;
        }
        const std::string type = trim(line.substr(second_separator + 1U));
        const zevryon::text::BidiBracketType expected = type == "o"
            ? zevryon::text::BidiBracketType::Open
            : type == "c" ? zevryon::text::BidiBracketType::Close
                           : zevryon::text::BidiBracketType::None;
        if (expected == zevryon::text::BidiBracketType::None || listed[codepoint]) {
            std::cerr << "invalid or duplicate bracket on line " << line_number << '\n';
            return 1;
        }
        const auto actual = zevryon::text::bidi_bracket_info(codepoint);
        if (actual.type != expected || actual.paired_codepoint != paired) {
            std::cerr << "bracket lookup mismatch on line " << line_number << '\n';
            return 1;
        }
        const auto reverse = zevryon::text::bidi_bracket_info(paired);
        const auto reverse_type = expected == zevryon::text::BidiBracketType::Open
            ? zevryon::text::BidiBracketType::Close
            : zevryon::text::BidiBracketType::Open;
        if (reverse.type != reverse_type || reverse.paired_codepoint != codepoint) {
            std::cerr << "non-reciprocal bracket on line " << line_number << '\n';
            return 1;
        }
        listed[codepoint] = true;
        ++records;
        if (expected == zevryon::text::BidiBracketType::Open) {
            ++opens;
        } else {
            ++closes;
        }
    }

    for (std::uint32_t codepoint = 0U; codepoint <= 0x10FFFFU; ++codepoint) {
        const auto info = zevryon::text::bidi_bracket_info(codepoint);
        if (listed[codepoint] !=
            (info.type != zevryon::text::BidiBracketType::None)) {
            std::cerr << "unexpected bracket property at U+" << std::hex
                      << codepoint << '\n';
            return 1;
        }
    }

    if (records != 128U || opens != 64U || closes != 64U) {
        std::cerr << "unexpected Unicode bracket totals\n";
        return 1;
    }

    std::cout << '{'
              << "\"schema\":\"zevryon.bidi-bracket-conformance.v1\","
              << "\"unicode_version\":\""
              << zevryon::text::kUnicodeBidiBracketVersion << "\","
              << "\"data_fingerprint\":\""
              << zevryon::text::kUnicodeBidiBracketFingerprint << "\","
              << "\"source_sha256\":\""
              << zevryon::text::kUnicodeBidiBracketSourceSha256 << "\","
              << "\"codepoints\":1114112,"
              << "\"records\":" << records << ','
              << "\"open\":" << opens << ','
              << "\"close\":" << closes << ','
              << "\"passed\":true}"
              << '\n';
    return 0;
}
