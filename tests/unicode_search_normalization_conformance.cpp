#include "unicode_search_normalization_data.generated.hpp"
#include "unicode_search_normalizer.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace zevryon::text;

bool parse_sequence(std::string_view field, std::vector<std::uint32_t>* output) {
    output->clear();
    std::size_t position = 0U;
    while (position < field.size()) {
        while (position < field.size() && field[position] == ' ') {
            ++position;
        }
        if (position == field.size()) {
            break;
        }
        const std::size_t start = position;
        while (position < field.size() && field[position] != ' ') {
            ++position;
        }
        std::uint32_t value = 0U;
        const char* begin = field.data() + static_cast<std::ptrdiff_t>(start);
        const char* end = field.data() + static_cast<std::ptrdiff_t>(position);
        const auto result = std::from_chars(begin, end, value, 16);
        if (result.ec != std::errc{} || result.ptr != end || value > 0x10ffffU) {
            return false;
        }
        output->push_back(value);
    }
    return true;
}

bool normalize(
    std::span<const std::uint32_t> input,
    bool compose,
    std::vector<std::uint32_t>* output,
    std::string* error_message) {
    std::vector<SearchSourceCodePoint> source;
    source.reserve(input.size());
    std::uint64_t offset = 0U;
    for (const std::uint32_t value : input) {
        source.push_back(SearchSourceCodePoint{value, offset, offset + 1U});
        ++offset;
    }

    UnicodeSearchNormalizerConfig config;
    config.max_pending_codepoints = 4096U;
    config.full_case_fold = false;
    config.compose = compose;
    UnicodeSearchNormalizer normalizer(kUnicodeSearchNormalizationTables, config);
    UnicodeSearchNormalizationError error;
    output->clear();
    const auto consumer = [output](std::span<const NormalizedSearchCodePoint> values) {
        for (const auto& value : values) {
            output->push_back(value.value);
        }
        return true;
    };
    if (!normalizer.feed(source, consumer, &error) || !normalizer.finish(consumer, &error)) {
        *error_message = std::string(error.message);
        return false;
    }
    return true;
}

bool check_equal(
    std::span<const std::uint32_t> input,
    std::span<const std::uint32_t> expected,
    bool compose,
    std::size_t line_number,
    std::string_view relation) {
    std::vector<std::uint32_t> actual;
    std::string error;
    if (!normalize(input, compose, &actual, &error)) {
        std::cerr << "normalization failure at line " << line_number << " (" << relation
                  << "): " << error << '\n';
        return false;
    }
    if (!std::equal(actual.begin(), actual.end(), expected.begin(), expected.end())) {
        std::cerr << "conformance mismatch at line " << line_number << " (" << relation << ")\nactual:";
        for (const auto value : actual) {
            std::cerr << " " << std::hex << value;
        }
        std::cerr << "\nexpected:";
        for (const auto value : expected) {
            std::cerr << " " << std::hex << value;
        }
        std::cerr << std::dec << "\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: unicode_search_normalization_conformance <NormalizationTest.txt>\n";
        return 2;
    }
    if (!validate_unicode_search_normalization_tables(kUnicodeSearchNormalizationTables)) {
        std::cerr << "generated Unicode normalization tables failed structural validation\n";
        return 2;
    }
    if (kUnicodeSearchNormalizationVersion != "17.0.0") {
        std::cerr << "unexpected Unicode normalization table version\n";
        return 2;
    }

    std::ifstream stream(argv[1]);
    if (!stream) {
        std::cerr << "cannot open NormalizationTest.txt\n";
        return 2;
    }

    std::string line;
    std::size_t line_number = 0U;
    std::uint64_t cases = 0U;
    std::array<std::vector<std::uint32_t>, 5> columns;
    while (std::getline(stream, line)) {
        ++line_number;
        const std::size_t hash = line.find('#');
        std::string_view content(line.data(), hash == std::string::npos ? line.size() : hash);
        const std::size_t first_non_space = content.find_first_not_of(" \t\r");
        if (first_non_space == std::string_view::npos || content[first_non_space] == '@') {
            continue;
        }
        content.remove_prefix(first_non_space);

        std::array<std::string_view, 5> fields{};
        std::size_t cursor = 0U;
        for (std::size_t index = 0U; index < fields.size(); ++index) {
            const std::size_t separator = content.find(';', cursor);
            if (separator == std::string_view::npos) {
                std::cerr << "invalid conformance row at line " << line_number << '\n';
                return 2;
            }
            std::string_view field = content.substr(cursor, separator - cursor);
            const std::size_t left = field.find_first_not_of(" \t");
            const std::size_t right = field.find_last_not_of(" \t");
            fields[index] = left == std::string_view::npos
                ? std::string_view{}
                : field.substr(left, right - left + 1U);
            cursor = separator + 1U;
        }
        for (std::size_t index = 0U; index < fields.size(); ++index) {
            if (!parse_sequence(fields[index], &columns[index])) {
                std::cerr << "invalid code-point sequence at line " << line_number << '\n';
                return 2;
            }
        }

        const auto& c1 = columns[0];
        const auto& c2 = columns[1];
        const auto& c3 = columns[2];
        const auto& c4 = columns[3];
        const auto& c5 = columns[4];
        const bool ok =
            check_equal(c1, c2, true, line_number, "NFC(c1)=c2") &&
            check_equal(c2, c2, true, line_number, "NFC(c2)=c2") &&
            check_equal(c3, c2, true, line_number, "NFC(c3)=c2") &&
            check_equal(c4, c4, true, line_number, "NFC(c4)=c4") &&
            check_equal(c5, c4, true, line_number, "NFC(c5)=c4") &&
            check_equal(c1, c3, false, line_number, "NFD(c1)=c3") &&
            check_equal(c2, c3, false, line_number, "NFD(c2)=c3") &&
            check_equal(c3, c3, false, line_number, "NFD(c3)=c3") &&
            check_equal(c4, c5, false, line_number, "NFD(c4)=c5") &&
            check_equal(c5, c5, false, line_number, "NFD(c5)=c5");
        if (!ok) {
            return 1;
        }
        ++cases;
    }

    if (!stream.eof()) {
        std::cerr << "failed while reading NormalizationTest.txt\n";
        return 2;
    }
    if (cases == 0U) {
        std::cerr << "no normalization conformance cases were executed\n";
        return 2;
    }
    std::cout << "Unicode 17 normalization conformance passed: " << cases << " cases\n";
    return 0;
}
