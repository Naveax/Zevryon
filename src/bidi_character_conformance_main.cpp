#include "bidi_resolver.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <sstream>
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

bool parse_unsigned(
    std::string_view text,
    int base,
    std::uint32_t* value) noexcept {
    if (value == nullptr || text.empty()) {
        return false;
    }
    try {
        const unsigned long parsed = std::stoul(std::string(text), nullptr, base);
        if (parsed > static_cast<unsigned long>(
                         std::numeric_limits<std::uint32_t>::max())) {
            return false;
        }
        *value = static_cast<std::uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_codepoints(
    std::string_view text,
    std::vector<zevryon::text::DecodedCodePoint>* codepoints) {
    codepoints->clear();
    std::istringstream stream{std::string(text)};
    std::string token;
    std::uint64_t source = 0U;
    while (stream >> token) {
        std::uint32_t value = 0U;
        if (!parse_unsigned(token, 16, &value) || value > 0x10ffffU ||
            (value >= 0xd800U && value <= 0xdfffU)) {
            return false;
        }
        const std::uint8_t length = value <= 0x7fU ? 1U
                                  : value <= 0x7ffU ? 2U
                                  : value <= 0xffffU ? 3U
                                  : 4U;
        codepoints->emplace_back(value, source, source + length, false);
        source += length;
    }
    return !codepoints->empty();
}

bool parse_levels(
    std::string_view text,
    std::vector<std::uint8_t>* levels) {
    levels->clear();
    std::istringstream stream{std::string(text)};
    std::string token;
    while (stream >> token) {
        if (token == "x") {
            levels->push_back(zevryon::text::kBidiRemovedLevel);
            continue;
        }
        std::uint32_t value = 0U;
        if (!parse_unsigned(token, 10, &value) || value > 125U) {
            return false;
        }
        levels->push_back(static_cast<std::uint8_t>(value));
    }
    return true;
}

bool parse_order(
    std::string_view text,
    std::vector<std::uint32_t>* order) {
    order->clear();
    std::istringstream stream{std::string(text)};
    std::string token;
    while (stream >> token) {
        std::uint32_t value = 0U;
        if (!parse_unsigned(token, 10, &value)) {
            return false;
        }
        order->push_back(value);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: zevryon-bidi-character-conformance BidiCharacterTest.txt\n";
        return 2;
    }
    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "failed to open BidiCharacterTest.txt\n";
        return 2;
    }

    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiBuffer,
        256U * 1024U);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiBuffer);
    std::pmr::vector<std::uint8_t> levels(&memory);
    std::pmr::vector<std::uint32_t> order(&memory);
    std::vector<zevryon::text::DecodedCodePoint> codepoints;
    std::vector<std::uint8_t> expected_levels;
    std::vector<std::uint32_t> expected_order;

    std::string line;
    std::size_t line_number = 0U;
    std::uint64_t tests = 0U;
    std::uint64_t total_codepoints = 0U;
    std::size_t maximum_input_units = 0U;
    std::uint8_t maximum_level = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        const std::size_t comment = line.find('#');
        const std::string_view content = trim(
            std::string_view(line).substr(0U, comment));
        if (content.empty()) {
            continue;
        }
        std::array<std::string_view, 5U> fields{};
        std::size_t field_index = 0U;
        std::size_t start = 0U;
        while (field_index < fields.size()) {
            const std::size_t separator = content.find(';', start);
            fields[field_index++] = trim(content.substr(
                start,
                separator == std::string_view::npos
                    ? std::string_view::npos
                    : separator - start));
            if (separator == std::string_view::npos) {
                break;
            }
            start = separator + 1U;
        }
        if (field_index != fields.size() ||
            !parse_codepoints(fields[0], &codepoints)) {
            std::cerr << "line " << line_number << ": invalid character test input\n";
            return 1;
        }
        std::uint32_t direction_value = 0U;
        std::uint32_t expected_paragraph_level = 0U;
        if (!parse_unsigned(fields[1], 10, &direction_value) ||
            direction_value > 2U ||
            !parse_unsigned(fields[2], 10, &expected_paragraph_level) ||
            expected_paragraph_level > 1U ||
            !parse_levels(fields[3], &expected_levels) ||
            !parse_order(fields[4], &expected_order) ||
            expected_levels.size() != codepoints.size()) {
            std::cerr << "line " << line_number << ": invalid expected result\n";
            return 1;
        }

        zevryon::text::BidiStats stats;
        zevryon::text::BidiError error;
        if (!zevryon::text::resolve_bidi_codepoints(
                codepoints,
                static_cast<zevryon::text::ParagraphDirection>(direction_value),
                &levels,
                &order,
                &stats,
                &error)) {
            std::cerr << "line " << line_number << ": resolution failed: "
                      << zevryon::text::bidi_error_kind_name(error.kind)
                      << " at " << error.input_index << ' '
                      << error.message << '\n';
            return 1;
        }
        if (stats.paragraph_level != expected_paragraph_level) {
            std::cerr << "line " << line_number << ": paragraph level mismatch\n";
            return 1;
        }
        if (levels.size() != expected_levels.size() ||
            !std::equal(levels.begin(), levels.end(), expected_levels.begin())) {
            std::cerr << "line " << line_number << ": level mismatch\n";
            return 1;
        }
        if (order.size() != expected_order.size() ||
            !std::equal(order.begin(), order.end(), expected_order.begin())) {
            std::cerr << "line " << line_number << ": reorder mismatch\n";
            return 1;
        }
        ++tests;
        total_codepoints += static_cast<std::uint64_t>(codepoints.size());
        maximum_input_units = std::max(maximum_input_units, codepoints.size());
        maximum_level = std::max(maximum_level, stats.maximum_resolved_level);
    }

    const auto resource =
        ledger.snapshot(zevryon::core::ResourceClass::BidiBuffer);
    if (tests == 0U || resource.rejected_reservations != 0U ||
        resource.accounting_errors != 0U ||
        !ledger.within_hard_limits() || !ledger.accounting_clean()) {
        std::cerr << "BidiCharacterTest resource or corpus contract failed\n";
        return 1;
    }

    std::cout << '{'
              << "\"schema\":\"zevryon.bidi-character-conformance.v1\","
              << "\"unicode_version\":\""
              << zevryon::text::kUnicodeBidiDataVersion << "\","
              << "\"data_fingerprint\":\""
              << zevryon::text::kUnicodeBidiDataFingerprint << "\","
              << "\"tests\":" << tests << ','
              << "\"total_codepoints\":" << total_codepoints << ','
              << "\"maximum_input_units\":" << maximum_input_units << ','
              << "\"maximum_resolved_level\":"
              << static_cast<unsigned int>(maximum_level) << ','
              << "\"bidi_current_bytes\":" << resource.current_bytes << ','
              << "\"bidi_peak_bytes\":" << resource.peak_bytes << ','
              << "\"passed\":true}\n";
    return 0;
}
