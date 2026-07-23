#include "unicode_bidi_mirroring.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kUnicodeScalarSpace = 0x110000U;
constexpr std::uint8_t kMappingPresent = 1U << 0U;
constexpr std::uint8_t kMappingBestFit = 1U << 1U;

std::vector<std::string> split_fields(const std::string& line, char delimiter) {
    std::vector<std::string> fields;
    std::size_t start = 0U;
    while (true) {
        const std::size_t end = line.find(delimiter, start);
        fields.push_back(line.substr(start, end - start));
        if (end == std::string::npos) {
            return fields;
        }
        start = end + 1U;
    }
}

std::uint32_t parse_hex(std::string_view text) {
    std::size_t consumed = 0U;
    const unsigned long value = std::stoul(std::string(text), &consumed, 16);
    if (consumed != text.size() || value >= kUnicodeScalarSpace) {
        throw std::runtime_error("invalid Unicode codepoint");
    }
    return static_cast<std::uint32_t>(value);
}

void parse_unicode_data(
    const char* path,
    std::vector<std::uint8_t>* mirrored,
    std::uint64_t* data_lines) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("unable to open UnicodeData.txt");
    }
    bool range_open = false;
    std::uint32_t range_first = 0U;
    bool range_mirrored = false;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        ++(*data_lines);
        const auto fields = split_fields(line, ';');
        if (fields.size() != 15U) {
            throw std::runtime_error("UnicodeData.txt line does not have 15 fields");
        }
        const std::uint32_t codepoint = parse_hex(fields[0]);
        const bool value = fields[9] == "Y";
        const bool first = fields[1].find(", First>") != std::string::npos;
        const bool last = fields[1].find(", Last>") != std::string::npos;
        if (first) {
            if (range_open) {
                throw std::runtime_error("nested UnicodeData range");
            }
            range_open = true;
            range_first = codepoint;
            range_mirrored = value;
            continue;
        }
        if (last) {
            if (!range_open || codepoint < range_first || value != range_mirrored) {
                throw std::runtime_error("malformed UnicodeData range");
            }
            if (range_mirrored) {
                for (std::uint32_t current = range_first; current <= codepoint; ++current) {
                    (*mirrored)[current] = 1U;
                }
            }
            range_open = false;
            continue;
        }
        if (range_open) {
            throw std::runtime_error("unterminated UnicodeData range");
        }
        (*mirrored)[codepoint] = value ? 1U : 0U;
    }
    if (range_open) {
        throw std::runtime_error("unterminated UnicodeData range at EOF");
    }
}

void parse_bidi_mirroring(
    const char* path,
    const std::vector<std::uint8_t>& mirrored,
    std::vector<std::uint32_t>* targets,
    std::vector<std::uint8_t>* mapping_flags,
    std::uint64_t* mapping_lines) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("unable to open BidiMirroring.txt");
    }
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t hash = line.find('#');
        const std::string comment = hash == std::string::npos
            ? std::string{}
            : line.substr(hash + 1U);
        const std::string data = line.substr(0U, hash);
        const auto fields = split_fields(data, ';');
        if (fields.empty() || fields[0].find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        if (fields.size() < 2U) {
            throw std::runtime_error("malformed BidiMirroring.txt record");
        }
        const auto trim = [](const std::string& value) {
            const std::size_t first = value.find_first_not_of(" \t\r");
            const std::size_t last = value.find_last_not_of(" \t\r");
            return value.substr(first, last - first + 1U);
        };
        const std::uint32_t source = parse_hex(trim(fields[0]));
        const std::uint32_t target = parse_hex(trim(fields[1]));
        if (mirrored[source] == 0U || ((*mapping_flags)[source] & kMappingPresent) != 0U) {
            throw std::runtime_error("invalid or duplicate BidiMirroring source");
        }
        (*targets)[source] = target;
        (*mapping_flags)[source] = static_cast<std::uint8_t>(
            kMappingPresent |
            (comment.find("[BEST FIT]") != std::string::npos
                 ? kMappingBestFit
                 : 0U));
        ++(*mapping_lines);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: zevryon-bidi-mirroring-conformance "
                     "UnicodeData.txt BidiMirroring.txt\n";
        return 2;
    }

    try {
        std::vector<std::uint8_t> expected_mirrored(kUnicodeScalarSpace, 0U);
        std::vector<std::uint32_t> expected_target(kUnicodeScalarSpace, 0U);
        std::vector<std::uint8_t> expected_mapping(kUnicodeScalarSpace, 0U);
        std::uint64_t unicode_data_lines = 0U;
        std::uint64_t mapping_lines = 0U;
        parse_unicode_data(argv[1], &expected_mirrored, &unicode_data_lines);
        parse_bidi_mirroring(
            argv[2],
            expected_mirrored,
            &expected_target,
            &expected_mapping,
            &mapping_lines);

        std::uint64_t mirrored_codepoints = 0U;
        std::uint64_t exact_mappings = 0U;
        std::uint64_t best_fit_mappings = 0U;
        std::uint64_t glyph_only = 0U;
        for (std::uint32_t codepoint = 0U;
             codepoint < static_cast<std::uint32_t>(kUnicodeScalarSpace);
             ++codepoint) {
            const auto actual = zevryon::text::bidi_mirroring_info(codepoint);
            const bool mirrored = expected_mirrored[codepoint] != 0U;
            const bool mapped =
                (expected_mapping[codepoint] & kMappingPresent) != 0U;
            const bool best_fit =
                (expected_mapping[codepoint] & kMappingBestFit) != 0U;
            if (actual.mirrored != mirrored ||
                actual.has_character_mapping != mapped ||
                actual.best_fit != best_fit ||
                actual.mirror_codepoint != (mapped ? expected_target[codepoint] : 0U)) {
                std::cerr << "mirroring mismatch at U+" << std::hex << codepoint << '\n';
                return 1;
            }
            if (!mirrored) {
                continue;
            }
            ++mirrored_codepoints;
            if (!mapped) {
                ++glyph_only;
            } else if (best_fit) {
                ++best_fit_mappings;
            } else {
                ++exact_mappings;
            }
        }

        std::cout << '{'
                  << "\"schema\":\"zevryon.bidi-mirroring-conformance.v1\","
                  << "\"unicode_version\":\"17.0.0\","
                  << "\"codepoints_scanned\":" << kUnicodeScalarSpace << ','
                  << "\"unicode_data_lines\":" << unicode_data_lines << ','
                  << "\"mapping_lines\":" << mapping_lines << ','
                  << "\"mirrored_codepoints\":" << mirrored_codepoints << ','
                  << "\"exact_mappings\":" << exact_mappings << ','
                  << "\"best_fit_mappings\":" << best_fit_mappings << ','
                  << "\"glyph_only_codepoints\":" << glyph_only << ','
                  << "\"passed\":true}\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "conformance failure: " << exception.what() << '\n';
        return 1;
    }
}
