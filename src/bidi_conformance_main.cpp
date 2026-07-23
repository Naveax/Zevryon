#include "unicode_bidi.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string_view trim(std::string_view value) noexcept {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

bool parse_hex(std::string_view text, std::uint32_t* output) noexcept {
    if (output == nullptr || text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = first + text.size();
    const auto result = std::from_chars(first, last, *output, 16);
    return result.ec == std::errc{} && result.ptr == last &&
           *output <= 0x10ffffU;
}

bool parse_range(
    std::string_view text,
    std::uint32_t* start,
    std::uint32_t* end) noexcept {
    const std::size_t separator = text.find("..");
    const std::string_view first = trim(text.substr(0U, separator));
    const std::string_view second = separator == std::string_view::npos
        ? first
        : trim(text.substr(separator + 2U));
    return parse_hex(first, start) && parse_hex(second, end) && *start <= *end;
}

bool parse_assignment(
    std::string_view content,
    std::uint32_t* start,
    std::uint32_t* end,
    zevryon::text::BidiClass* value) noexcept {
    const std::size_t separator = content.find(';');
    if (separator == std::string_view::npos) {
        return false;
    }
    return parse_range(trim(content.substr(0U, separator)), start, end) &&
           zevryon::text::bidi_class_from_name(
               trim(content.substr(separator + 1U)),
               value);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: zevryon-bidi-conformance DerivedBidiClass.txt\n";
        return 2;
    }

    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "failed to open DerivedBidiClass.txt\n";
        return 1;
    }

    constexpr std::size_t kCodepointCount = 0x110000U;
    std::vector<zevryon::text::BidiClass> expected(
        kCodepointCount,
        zevryon::text::BidiClass::L);
    std::size_t explicit_ranges = 0U;
    std::size_t missing_ranges = 0U;
    std::string line;

    while (std::getline(input, line)) {
        const std::string_view view(line);
        const std::size_t missing = view.find("@missing:");
        if (missing != std::string_view::npos) {
            std::string_view content = view.substr(missing + 9U);
            const std::size_t comment = content.find('#');
            content = trim(content.substr(0U, comment));
            std::uint32_t start = 0U;
            std::uint32_t end = 0U;
            zevryon::text::BidiClass value = zevryon::text::BidiClass::L;
            if (!parse_assignment(content, &start, &end, &value)) {
                std::cerr << "invalid @missing line: " << line << '\n';
                return 1;
            }
            for (std::uint32_t codepoint = start;; ++codepoint) {
                expected[codepoint] = value;
                if (codepoint == end) {
                    break;
                }
            }
            ++missing_ranges;
            continue;
        }

        const std::size_t comment = view.find('#');
        const std::string_view content = trim(view.substr(0U, comment));
        if (content.empty()) {
            continue;
        }
        std::uint32_t start = 0U;
        std::uint32_t end = 0U;
        zevryon::text::BidiClass value = zevryon::text::BidiClass::L;
        if (!parse_assignment(content, &start, &end, &value)) {
            std::cerr << "invalid DerivedBidiClass line: " << line << '\n';
            return 1;
        }
        for (std::uint32_t codepoint = start;; ++codepoint) {
            expected[codepoint] = value;
            if (codepoint == end) {
                break;
            }
        }
        ++explicit_ranges;
    }

    for (std::uint32_t codepoint = 0U; codepoint <= 0x10ffffU; ++codepoint) {
        const zevryon::text::BidiClass actual =
            zevryon::text::bidi_class_of(codepoint);
        if (actual != expected[codepoint]) {
            std::cerr << "Bidi_Class mismatch at U+" << std::hex << codepoint
                      << ": expected "
                      << zevryon::text::bidi_class_short_name(expected[codepoint])
                      << " got "
                      << zevryon::text::bidi_class_short_name(actual)
                      << '\n';
            return 1;
        }
    }

    std::cout << '{'
              << "\"schema\":\"zevryon.bidi-conformance.v1\","
              << "\"unicode_version\":\""
              << zevryon::text::kUnicodeBidiDataVersion << "\","
              << "\"data_fingerprint\":\""
              << zevryon::text::kUnicodeBidiDataFingerprint << "\","
              << "\"codepoints\":1114112,"
              << "\"explicit_ranges\":" << explicit_ranges << ','
              << "\"missing_ranges\":" << missing_ranges << ','
              << "\"generated_ranges\":"
              << zevryon::text::kBidiClassRanges.size() << ','
              << "\"passed\":true}\n";
    return 0;
}
