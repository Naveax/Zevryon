#include "bidi_implicit.hpp"
#include "bidi_neutral.hpp"
#include "bidi_weak.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cctype>
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

using zevryon::text::BidiClass;
using zevryon::text::BidiExplicitUnit;
using zevryon::text::BidiImplicitError;
using zevryon::text::BidiImplicitStats;
using zevryon::text::BidiIsolatingRunSequence;
using zevryon::text::BidiLevelRun;
using zevryon::text::BidiNeutralError;
using zevryon::text::BidiNeutralStats;
using zevryon::text::BidiSequenceTopology;
using zevryon::text::BidiWeakError;
using zevryon::text::BidiWeakStats;
using zevryon::text::DecodedCodePoint;

std::string trim(std::string value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

bool parse_type(std::string_view token, BidiClass* value) noexcept {
    if (value == nullptr) {
        return false;
    }
    if (token == "L") {
        *value = BidiClass::L;
    } else if (token == "R") {
        *value = BidiClass::R;
    } else if (token == "AL") {
        *value = BidiClass::AL;
    } else if (token == "EN") {
        *value = BidiClass::EN;
    } else if (token == "ES") {
        *value = BidiClass::ES;
    } else if (token == "ET") {
        *value = BidiClass::ET;
    } else if (token == "AN") {
        *value = BidiClass::AN;
    } else if (token == "CS") {
        *value = BidiClass::CS;
    } else if (token == "NSM") {
        *value = BidiClass::NSM;
    } else if (token == "ON") {
        *value = BidiClass::ON;
    } else {
        return false;
    }
    return true;
}

std::uint8_t paragraph_level(
    unsigned mode,
    const std::vector<BidiClass>& types) noexcept {
    if (mode == 2U) {
        return 0U;
    }
    if (mode == 4U) {
        return 1U;
    }
    for (const BidiClass type : types) {
        if (type == BidiClass::L) {
            return 0U;
        }
        if (type == BidiClass::R || type == BidiClass::AL) {
            return 1U;
        }
    }
    return 0U;
}

bool run_case(
    const std::vector<BidiClass>& original_types,
    std::uint8_t base_level,
    const std::vector<int>& expected,
    std::size_t line_number,
    unsigned mode) {
    std::vector<DecodedCodePoint> codepoints;
    std::vector<BidiExplicitUnit> units;
    codepoints.reserve(original_types.size());
    units.reserve(original_types.size());
    BidiSequenceTopology topology(std::pmr::get_default_resource());
    for (std::size_t index = 0U; index < original_types.size(); ++index) {
        codepoints.emplace_back(
            static_cast<std::uint32_t>('x'),
            static_cast<std::uint64_t>(index),
            static_cast<std::uint64_t>(index + 1U),
            false);
        units.push_back(BidiExplicitUnit{
            static_cast<std::uint64_t>(index),
            static_cast<std::uint32_t>(index),
            original_types[index],
            original_types[index],
            base_level,
            0U});
        topology.active_unit_indices.push_back(
            static_cast<std::uint32_t>(index));
    }
    if (!original_types.empty()) {
        topology.level_runs.push_back(BidiLevelRun{
            0U,
            static_cast<std::uint32_t>(original_types.size()),
            base_level,
            0U,
            0U});
        topology.sequence_run_indices.push_back(0U);
        topology.sequences.push_back(BidiIsolatingRunSequence{
            0U,
            1U,
            (base_level & 1U) == 0U ? BidiClass::L : BidiClass::R,
            (base_level & 1U) == 0U ? BidiClass::L : BidiClass::R,
            base_level,
            0U,
            0U});
    }

    zevryon::core::ResourceLedger weak_ledger;
    weak_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiTypeResolution,
        4096U);
    zevryon::core::LedgerMemoryResource weak_memory(
        weak_ledger,
        zevryon::core::ResourceClass::BidiTypeResolution);
    std::pmr::vector<BidiClass> weak_types(&weak_memory);
    BidiWeakStats weak_stats;
    BidiWeakError weak_error;
    if (!zevryon::text::resolve_bidi_weak_types(
            units,
            topology,
            &weak_types,
            &weak_stats,
            &weak_error)) {
        std::cerr << "weak resolver rejected BidiTest line " << line_number
                  << " mode " << mode << ": " << weak_error.message << '\n';
        return false;
    }

    zevryon::core::ResourceLedger neutral_ledger;
    neutral_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiNeutralResolution,
        4096U);
    zevryon::core::LedgerMemoryResource neutral_memory(
        neutral_ledger,
        zevryon::core::ResourceClass::BidiNeutralResolution);
    std::pmr::vector<BidiClass> neutral_types(&neutral_memory);
    BidiNeutralStats neutral_stats;
    BidiNeutralError neutral_error;
    if (!zevryon::text::resolve_bidi_neutral_types(
            codepoints,
            units,
            topology,
            weak_types,
            &neutral_types,
            &neutral_stats,
            &neutral_error)) {
        std::cerr << "neutral resolver rejected BidiTest line " << line_number
                  << " mode " << mode << ": " << neutral_error.message << '\n';
        return false;
    }

    zevryon::core::ResourceLedger implicit_ledger;
    implicit_ledger.set_hard_limit(
        zevryon::core::ResourceClass::BidiImplicitLevel,
        4096U);
    zevryon::core::LedgerMemoryResource implicit_memory(
        implicit_ledger,
        zevryon::core::ResourceClass::BidiImplicitLevel);
    std::pmr::vector<std::uint8_t> output(&implicit_memory);
    BidiImplicitStats implicit_stats;
    BidiImplicitError implicit_error;
    if (!zevryon::text::resolve_bidi_implicit_levels(
            units,
            topology,
            neutral_types,
            &output,
            &implicit_stats,
            &implicit_error)) {
        std::cerr << "implicit resolver rejected BidiTest line " << line_number
                  << " mode " << mode << ": " << implicit_error.message << '\n';
        return false;
    }
    if (output.size() != expected.size()) {
        std::cerr << "level count mismatch at BidiTest line " << line_number
                  << " mode " << mode << '\n';
        return false;
    }
    for (std::size_t index = 0U; index < output.size(); ++index) {
        if (expected[index] < 0 ||
            output[index] != static_cast<std::uint8_t>(expected[index])) {
            std::cerr << "level mismatch at BidiTest line " << line_number
                      << " mode " << mode << " index " << index
                      << ": expected " << expected[index]
                      << " got " << static_cast<unsigned>(output[index]) << '\n';
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: zevryon-bidi-implicit-conformance BidiTest.txt\n";
        return 2;
    }
    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "unable to open BidiTest data\n";
        return 2;
    }

    std::vector<int> expected_levels;
    std::uint64_t data_lines = 0U;
    std::uint64_t filtered_lines = 0U;
    std::uint64_t cases = 0U;
    std::uint64_t auto_cases = 0U;
    std::uint64_t ltr_cases = 0U;
    std::uint64_t rtl_cases = 0U;
    std::string raw;
    std::size_t line_number = 0U;
    while (std::getline(input, raw)) {
        ++line_number;
        const std::size_t comment = raw.find('#');
        std::string line = trim(raw.substr(0U, comment));
        if (line.empty()) {
            continue;
        }
        if (line.rfind("@Levels:", 0U) == 0U) {
            expected_levels.clear();
            std::istringstream values(line.substr(8U));
            std::string token;
            while (values >> token) {
                expected_levels.push_back(token == "x" ? -1 : std::stoi(token));
            }
            continue;
        }
        if (line.front() == '@') {
            continue;
        }

        ++data_lines;
        const std::size_t separator = line.find(';');
        if (separator == std::string::npos || expected_levels.empty()) {
            std::cerr << "invalid BidiTest structure at line " << line_number << '\n';
            return 2;
        }
        std::istringstream type_stream(line.substr(0U, separator));
        std::vector<BidiClass> types;
        std::string token;
        bool supported = true;
        while (type_stream >> token) {
            BidiClass type = BidiClass::L;
            if (!parse_type(token, &type)) {
                supported = false;
                break;
            }
            types.push_back(type);
        }
        if (!supported || types.size() != expected_levels.size()) {
            continue;
        }

        const std::string bitset_text = trim(line.substr(separator + 1U));
        unsigned bitset = 0U;
        try {
            bitset = static_cast<unsigned>(std::stoul(bitset_text, nullptr, 16));
        } catch (...) {
            std::cerr << "invalid BidiTest paragraph bitset at line "
                      << line_number << '\n';
            return 2;
        }
        ++filtered_lines;
        constexpr std::array<unsigned, 3> modes{1U, 2U, 4U};
        for (const unsigned mode : modes) {
            if ((bitset & mode) == 0U) {
                continue;
            }
            if (!run_case(
                    types,
                    paragraph_level(mode, types),
                    expected_levels,
                    line_number,
                    mode)) {
                return 1;
            }
            ++cases;
            auto_cases += mode == 1U ? 1U : 0U;
            ltr_cases += mode == 2U ? 1U : 0U;
            rtl_cases += mode == 4U ? 1U : 0U;
        }
    }

    if (filtered_lines == 0U || cases == 0U) {
        std::cerr << "BidiTest contained no bounded W-N-I subset cases\n";
        return 1;
    }
    std::cout << '{'
              << "\"schema\":\"zevryon.bidi-implicit-conformance.v1\","
              << "\"unicode_version\":\"17.0.0\","
              << "\"uax_revision\":51,"
              << "\"data_lines\":" << data_lines << ','
              << "\"filtered_lines\":" << filtered_lines << ','
              << "\"cases\":" << cases << ','
              << "\"auto_ltr_cases\":" << auto_cases << ','
              << "\"ltr_cases\":" << ltr_cases << ','
              << "\"rtl_cases\":" << rtl_cases << ','
              << "\"passed\":true}\n";
    return 0;
}
