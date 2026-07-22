#include "grapheme_segmenter.hpp"
#include "ledger_memory_resource.hpp"
#include "unicode_grapheme_data.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory_resource>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::uint8_t utf8_length(std::uint32_t value) noexcept {
    return value <= 0x7fU ? 1U
         : value <= 0x7ffU ? 2U
         : value <= 0xffffU ? 3U
         : 4U;
}

bool parse_hex(std::string_view token, std::uint32_t* value) {
    if (value == nullptr || token.empty()) {
        return false;
    }
    const char* begin = token.data();
    const char* end = begin + token.size();
    const auto parsed = std::from_chars(begin, end, *value, 16);
    return parsed.ec == std::errc{} && parsed.ptr == end &&
           *value <= 0x10ffffU;
}

struct TestCase {
    std::vector<std::uint32_t> values;
    std::vector<std::uint32_t> expected_cluster_starts;
};

bool parse_case(const std::string& input, TestCase* test_case) {
    if (test_case == nullptr) {
        return false;
    }
    test_case->values.clear();
    test_case->expected_cluster_starts.clear();
    const std::size_t comment = input.find('#');
    const std::string content = input.substr(0U, comment);
    std::istringstream stream(content);
    std::string marker;
    if (!(stream >> marker)) {
        return false;
    }
    if (marker != "÷") {
        return false;
    }

    while (true) {
        std::string codepoint_token;
        if (!(stream >> codepoint_token)) {
            break;
        }
        std::uint32_t value = 0U;
        if (!parse_hex(codepoint_token, &value)) {
            return false;
        }
        if (marker == "÷") {
            test_case->expected_cluster_starts.push_back(
                static_cast<std::uint32_t>(test_case->values.size()));
        }
        test_case->values.push_back(value);
        if (!(stream >> marker) || (marker != "÷" && marker != "×")) {
            return false;
        }
    }
    return !test_case->values.empty() && marker == "÷";
}

bool run_case(
    const TestCase& test_case,
    std::size_t line_number,
    std::uint64_t* cluster_total) {
    std::vector<zevryon::text::DecodedCodePoint> codepoints;
    codepoints.reserve(test_case.values.size());
    std::uint64_t source = 0U;
    for (const std::uint32_t value : test_case.values) {
        const std::uint8_t length = utf8_length(value);
        codepoints.emplace_back(value, source, source + length, false);
        source += length;
    }

    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(
        zevryon::core::ResourceClass::GraphemeCluster,
        64U * 1024U);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::GraphemeCluster);
    std::pmr::vector<zevryon::text::GraphemeCluster> clusters(&memory);
    zevryon::text::GraphemeSegmentStats stats;
    zevryon::text::GraphemeError error;
    if (!zevryon::text::segment_graphemes(
            codepoints,
            &clusters,
            &stats,
            &error)) {
        std::cerr << "line " << line_number << ": segmentation failed: "
                  << zevryon::text::grapheme_error_kind_name(error.kind)
                  << " at codepoint " << error.codepoint_index << ' '
                  << error.message << '\n';
        return false;
    }
    if (clusters.size() != test_case.expected_cluster_starts.size()) {
        std::cerr << "line " << line_number << ": cluster count mismatch, expected "
                  << test_case.expected_cluster_starts.size() << " got "
                  << clusters.size() << '\n';
        return false;
    }
    for (std::size_t index = 0U; index < clusters.size(); ++index) {
        if (clusters[index].first_codepoint !=
            test_case.expected_cluster_starts[index]) {
            std::cerr << "line " << line_number << ": cluster " << index
                      << " starts at " << clusters[index].first_codepoint
                      << ", expected "
                      << test_case.expected_cluster_starts[index] << '\n';
            return false;
        }
        const std::uint32_t expected_end =
            index + 1U < clusters.size()
                ? test_case.expected_cluster_starts[index + 1U]
                : static_cast<std::uint32_t>(test_case.values.size());
        if (clusters[index].codepoint_count !=
            expected_end - test_case.expected_cluster_starts[index]) {
            std::cerr << "line " << line_number << ": cluster " << index
                      << " length mismatch\n";
            return false;
        }
    }
    if (!ledger.within_hard_limits() || !ledger.accounting_clean()) {
        std::cerr << "line " << line_number
                  << ": grapheme resource accounting failed\n";
        return false;
    }
    *cluster_total += static_cast<std::uint64_t>(clusters.size());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: zevryon-grapheme-conformance GraphemeBreakTest.txt\n";
        return 2;
    }
    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "failed to open GraphemeBreakTest file\n";
        return 2;
    }

    std::string line;
    std::size_t line_number = 0U;
    std::uint64_t test_count = 0U;
    std::uint64_t codepoint_total = 0U;
    std::uint64_t cluster_total = 0U;
    TestCase test_case;
    while (std::getline(input, line)) {
        ++line_number;
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        if (!parse_case(line, &test_case)) {
            std::cerr << "line " << line_number << ": invalid conformance syntax\n";
            return 1;
        }
        if (!run_case(test_case, line_number, &cluster_total)) {
            return 1;
        }
        ++test_count;
        codepoint_total += static_cast<std::uint64_t>(test_case.values.size());
    }
    if (test_count == 0U) {
        std::cerr << "conformance file contained no tests\n";
        return 1;
    }

    std::cout << '{'
              << "\"schema\":\"zevryon.grapheme-conformance.v1\","
              << "\"unicode_version\":\""
              << zevryon::text::kUnicodeGraphemeDataVersion << "\","
              << "\"data_fingerprint\":\""
              << zevryon::text::kUnicodeGraphemeDataFingerprint << "\","
              << "\"tests\":" << test_count << ','
              << "\"codepoints\":" << codepoint_total << ','
              << "\"clusters\":" << cluster_total << ','
              << "\"passed\":true}"
              << '\n';
    return 0;
}
