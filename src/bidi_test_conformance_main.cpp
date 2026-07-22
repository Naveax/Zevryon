#include "bidi_resolver.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_bidi.hpp"

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
#include <utility>
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
        try {
            const unsigned long value = std::stoul(token);
            if (value > 125UL) {
                return false;
            }
            levels->push_back(static_cast<std::uint8_t>(value));
        } catch (...) {
            return false;
        }
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
        try {
            const unsigned long value = std::stoul(token);
            if (value > static_cast<unsigned long>(
                            std::numeric_limits<std::uint32_t>::max())) {
                return false;
            }
            order->push_back(static_cast<std::uint32_t>(value));
        } catch (...) {
            return false;
        }
    }
    return true;
}

bool parse_classes(
    std::string_view text,
    std::vector<zevryon::text::BidiClass>* classes) {
    classes->clear();
    std::istringstream stream{std::string(text)};
    std::string token;
    while (stream >> token) {
        zevryon::text::BidiClass value;
        if (!zevryon::text::bidi_class_from_name(token, &value)) {
            return false;
        }
        classes->push_back(value);
    }
    return !classes->empty();
}

bool run_case(
    const std::vector<zevryon::text::BidiClass>& classes,
    zevryon::text::ParagraphDirection direction,
    const std::vector<std::uint8_t>& expected_levels,
    const std::vector<std::uint32_t>& expected_order,
    std::pmr::vector<std::uint8_t>* levels,
    std::pmr::vector<std::uint32_t>* order,
    std::size_t line_number,
    std::uint8_t* maximum_level) {
    zevryon::text::BidiStats stats;
    zevryon::text::BidiError error;
    if (!zevryon::text::resolve_bidi_classes(
            classes,
            direction,
            levels,
            order,
            &stats,
            &error)) {
        std::cerr << "line " << line_number << ": resolution failed: "
                  << zevryon::text::bidi_error_kind_name(error.kind)
                  << " at " << error.input_index << ' '
                  << error.message << '\n';
        return false;
    }
    if (levels->size() != expected_levels.size() ||
        !std::equal(levels->begin(), levels->end(), expected_levels.begin())) {
        std::cerr << "line " << line_number << ": level mismatch for mode "
                  << static_cast<unsigned int>(direction) << '\n';
        return false;
    }
    if (order->size() != expected_order.size() ||
        !std::equal(order->begin(), order->end(), expected_order.begin())) {
        std::cerr << "line " << line_number << ": reorder mismatch for mode "
                  << static_cast<unsigned int>(direction) << '\n';
        return false;
    }
    *maximum_level = std::max(*maximum_level, stats.maximum_resolved_level);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: zevryon-bidi-test-conformance BidiTest.txt\n";
        return 2;
    }
    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "failed to open BidiTest.txt\n";
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
    std::vector<std::uint8_t> expected_levels;
    std::vector<std::uint32_t> expected_order;
    std::vector<zevryon::text::BidiClass> classes;

    std::string line;
    std::size_t line_number = 0U;
    std::uint64_t data_lines = 0U;
    std::uint64_t resolved_cases = 0U;
    std::size_t maximum_input_units = 0U;
    std::uint8_t maximum_level = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        const std::string_view content = trim(line);
        if (content.empty() || content.front() == '#') {
            continue;
        }
        if (content.starts_with("@Levels:")) {
            if (!parse_levels(trim(content.substr(8U)), &expected_levels)) {
                std::cerr << "line " << line_number << ": invalid @Levels\n";
                return 1;
            }
            continue;
        }
        if (content.starts_with("@Reorder:")) {
            if (!parse_order(trim(content.substr(9U)), &expected_order)) {
                std::cerr << "line " << line_number << ": invalid @Reorder\n";
                return 1;
            }
            continue;
        }
        if (content.front() == '@') {
            continue;
        }
        const std::size_t separator = content.find(';');
        if (separator == std::string_view::npos ||
            !parse_classes(trim(content.substr(0U, separator)), &classes)) {
            std::cerr << "line " << line_number << ": invalid test input\n";
            return 1;
        }
        unsigned long bitset = 0UL;
        try {
            bitset = std::stoul(
                std::string(trim(content.substr(separator + 1U))),
                nullptr,
                16);
        } catch (...) {
            std::cerr << "line " << line_number << ": invalid paragraph bitset\n";
            return 1;
        }
        if (expected_levels.size() != classes.size()) {
            std::cerr << "line " << line_number << ": expected level arity mismatch\n";
            return 1;
        }
        ++data_lines;
        maximum_input_units = std::max(maximum_input_units, classes.size());
        const std::array<
            std::pair<unsigned long, zevryon::text::ParagraphDirection>,
            3U> modes{{
                {1UL, zevryon::text::ParagraphDirection::Auto},
                {2UL, zevryon::text::ParagraphDirection::LeftToRight},
                {4UL, zevryon::text::ParagraphDirection::RightToLeft},
            }};
        for (const auto& mode : modes) {
            if ((bitset & mode.first) == 0UL) {
                continue;
            }
            if (!run_case(
                    classes,
                    mode.second,
                    expected_levels,
                    expected_order,
                    &levels,
                    &order,
                    line_number,
                    &maximum_level)) {
                return 1;
            }
            ++resolved_cases;
        }
    }

    const auto resource =
        ledger.snapshot(zevryon::core::ResourceClass::BidiBuffer);
    if (data_lines == 0U || resolved_cases == 0U ||
        resource.rejected_reservations != 0U ||
        resource.accounting_errors != 0U ||
        !ledger.within_hard_limits() || !ledger.accounting_clean()) {
        std::cerr << "BidiTest resource or corpus contract failed\n";
        return 1;
    }

    std::cout << '{'
              << "\"schema\":\"zevryon.bidi-test-conformance.v1\","
              << "\"unicode_version\":\""
              << zevryon::text::kUnicodeBidiDataVersion << "\","
              << "\"data_fingerprint\":\""
              << zevryon::text::kUnicodeBidiDataFingerprint << "\","
              << "\"data_lines\":" << data_lines << ','
              << "\"resolved_cases\":" << resolved_cases << ','
              << "\"maximum_input_units\":" << maximum_input_units << ','
              << "\"maximum_resolved_level\":"
              << static_cast<unsigned int>(maximum_level) << ','
              << "\"bidi_current_bytes\":" << resource.current_bytes << ','
              << "\"bidi_peak_bytes\":" << resource.peak_bytes << ','
              << "\"passed\":true}\n";
    return 0;
}
