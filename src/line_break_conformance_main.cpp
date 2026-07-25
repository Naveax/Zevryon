#include "line_break_opportunity.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kBreakMarker = "\xC3\xB7";
constexpr std::string_view kNoBreakMarker = "\xC3\x97";

std::uint8_t utf8_length(std::uint32_t value) noexcept {
    if (value <= 0x7FU) {
        return 1U;
    }
    if (value <= 0x7FFU) {
        return 2U;
    }
    if (value <= 0xFFFFU) {
        return 3U;
    }
    return 4U;
}

bool parse_marker(const std::string& token, bool* break_allowed) {
    if (token == kBreakMarker) {
        *break_allowed = true;
        return true;
    }
    if (token == kNoBreakMarker) {
        *break_allowed = false;
        return true;
    }
    return false;
}

struct ParsedCase final {
    std::vector<std::uint32_t> codepoints;
    std::vector<bool> breaks;
};

bool parse_case(std::string line, ParsedCase* output, std::string* error) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
        line.resize(comment);
    }
    std::istringstream stream(line);
    std::string marker;
    if (!(stream >> marker)) {
        return false;
    }
    bool break_allowed = false;
    if (!parse_marker(marker, &break_allowed)) {
        *error = "test line does not start with a break marker";
        return false;
    }
    output->breaks.push_back(break_allowed);

    std::string codepoint_token;
    while (stream >> codepoint_token) {
        std::uint32_t value = 0U;
        try {
            std::size_t consumed = 0U;
            const unsigned long parsed = std::stoul(
                codepoint_token,
                &consumed,
                16);
            if (consumed != codepoint_token.size() || parsed > 0x10FFFFUL) {
                *error = "invalid Unicode scalar token";
                return false;
            }
            value = static_cast<std::uint32_t>(parsed);
        } catch (...) {
            *error = "invalid Unicode scalar token";
            return false;
        }
        output->codepoints.push_back(value);

        if (!(stream >> marker) || !parse_marker(marker, &break_allowed)) {
            *error = "code point is not followed by a break marker";
            return false;
        }
        output->breaks.push_back(break_allowed);
    }
    if (output->breaks.size() != output->codepoints.size() + 1U) {
        *error = "test line has an invalid marker count";
        return false;
    }
    return true;
}

void build_singleton_fixture(
    const ParsedCase& input,
    std::vector<zevryon::text::DecodedCodePoint>* codepoints,
    std::vector<zevryon::text::GraphemeBoundary>* boundaries) {
    std::uint64_t source_offset = 0U;
    codepoints->reserve(input.codepoints.size());
    boundaries->reserve(input.codepoints.size() + 1U);
    for (std::size_t index = 0U; index < input.codepoints.size(); ++index) {
        boundaries->push_back({
            source_offset,
            static_cast<std::uint32_t>(index)});
        const std::uint8_t length = utf8_length(input.codepoints[index]);
        codepoints->push_back({
            input.codepoints[index],
            source_offset,
            source_offset + length,
            false});
        source_offset += length;
    }
    if (!input.codepoints.empty()) {
        boundaries->push_back({
            source_offset,
            static_cast<std::uint32_t>(input.codepoints.size())});
    }
}

std::string describe_case(const ParsedCase& test) {
    std::ostringstream output;
    for (std::size_t index = 0U; index < test.codepoints.size(); ++index) {
        output << (test.breaks[index] ? "÷ " : "× ")
               << std::uppercase << std::hex << std::setw(4)
               << std::setfill('0') << test.codepoints[index] << ' ';
    }
    output << (test.breaks.back() ? "÷" : "×");
    return output.str();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: zevryon-line-break-conformance <LineBreakTest.txt>\n";
        return 2;
    }

    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "failed to open LineBreakTest file\n";
        return 2;
    }

    std::uint64_t tests = 0U;
    std::uint64_t codepoint_total = 0U;
    std::uint64_t boundary_total = 0U;
    std::uint64_t failures = 0U;
    std::uint64_t build_failures = 0U;
    std::uint64_t parse_failures = 0U;
    std::uint64_t mandatory_boundaries = 0U;
    std::uint64_t line_number = 0U;
    std::string line;
    while (std::getline(input, line)) {
        ++line_number;
        ParsedCase test;
        std::string parse_error;
        const bool parsed = parse_case(line, &test, &parse_error);
        if (!parsed) {
            const std::size_t first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos || line[first] == '#') {
                continue;
            }
            ++parse_failures;
            if (parse_failures <= 20U) {
                std::cerr << "line " << line_number << ": "
                          << parse_error << '\n';
            }
            continue;
        }

        ++tests;
        codepoint_total += test.codepoints.size();
        boundary_total += test.breaks.size();
        std::vector<zevryon::text::DecodedCodePoint> codepoints;
        std::vector<zevryon::text::GraphemeBoundary> boundaries;
        build_singleton_fixture(test, &codepoints, &boundaries);

        zevryon::text::LineBreakOpportunityMap map;
        zevryon::text::LineBreakOpportunityStats stats;
        zevryon::text::LineBreakOpportunityError error;
        const bool built = zevryon::text::build_line_break_opportunity_map(
            {
                std::span<const zevryon::text::DecodedCodePoint>(codepoints),
                std::span<const zevryon::text::GraphemeBoundary>(boundaries)
            },
            &map,
            &stats,
            &error);
        if (!built) {
            ++failures;
            ++build_failures;
            if (failures <= 20U) {
                std::cerr << "line " << line_number << " build failure: "
                          << zevryon::text::line_break_opportunity_error_kind_name(
                                 error.kind)
                          << ": " << error.message << " | "
                          << describe_case(test) << '\n';
            }
            continue;
        }

        bool failed = map.opportunities.size() != test.breaks.size();
        std::size_t mismatch = 0U;
        if (!failed) {
            for (std::size_t boundary = 0U;
                 boundary < test.breaks.size();
                 ++boundary) {
                const auto actual = static_cast<zevryon::text::LineBreakOpportunity>(
                    map.opportunities[boundary]);
                const bool actual_break =
                    actual != zevryon::text::LineBreakOpportunity::Prohibited;
                if (actual == zevryon::text::LineBreakOpportunity::Mandatory) {
                    ++mandatory_boundaries;
                }
                if (actual_break != test.breaks[boundary]) {
                    failed = true;
                    mismatch = boundary;
                    break;
                }
            }
        }
        if (failed) {
            ++failures;
            if (failures <= 20U) {
                std::cerr << "line " << line_number
                          << " mismatch at boundary " << mismatch << " | "
                          << describe_case(test) << '\n';
            }
        }
    }

    std::cout << "{\n"
              << "  \"schema\": \"zevryon.line-break-conformance.v1\",\n"
              << "  \"unicode_version\": \""
              << zevryon::text::kUnicodeLineBreakDataVersion << "\",\n"
              << "  \"tests\": " << tests << ",\n"
              << "  \"codepoints\": " << codepoint_total << ",\n"
              << "  \"boundaries\": " << boundary_total << ",\n"
              << "  \"mandatory_boundaries\": " << mandatory_boundaries << ",\n"
              << "  \"parse_failures\": " << parse_failures << ",\n"
              << "  \"build_failures\": " << build_failures << ",\n"
              << "  \"failures\": " << failures << ",\n"
              << "  \"passed\": "
              << (failures == 0U && parse_failures == 0U ? "true" : "false")
              << "\n}\n";
    return failures == 0U && parse_failures == 0U ? 0 : 1;
}
