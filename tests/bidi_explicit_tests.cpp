#include "bidi_explicit.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_bidi.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

using zevryon::text::BidiClass;
using zevryon::text::BidiExplicitError;
using zevryon::text::BidiExplicitErrorKind;
using zevryon::text::BidiExplicitStats;
using zevryon::text::BidiExplicitUnit;
using zevryon::text::BidiParagraphDirection;
using zevryon::text::DecodedCodePoint;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

std::uint8_t utf8_length(std::uint32_t value) noexcept {
    return value <= 0x7fU ? 1U
         : value <= 0x7ffU ? 2U
         : value <= 0xffffU ? 3U
         : 4U;
}

std::vector<DecodedCodePoint> make_codepoints(
    std::initializer_list<std::uint32_t> values,
    std::uint64_t source_base = 0U) {
    std::vector<DecodedCodePoint> output;
    output.reserve(values.size());
    std::uint64_t source = source_base;
    for (const std::uint32_t value : values) {
        const std::uint8_t length = utf8_length(value);
        output.emplace_back(value, source, source + length, false);
        source += length;
    }
    return output;
}

bool resolve(
    const std::vector<DecodedCodePoint>& codepoints,
    BidiParagraphDirection direction,
    std::vector<BidiExplicitUnit>* output,
    BidiExplicitStats* stats,
    BidiExplicitError* error,
    std::size_t budget = 1U << 20U,
    zevryon::core::ResourceSnapshot* snapshot = nullptr) {
    zevryon::core::ResourceLedger ledger;
    ledger.set_hard_limit(zevryon::core::ResourceClass::BidiRun, budget);
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiRun);
    std::pmr::vector<BidiExplicitUnit> units(&memory);
    const bool result = zevryon::text::resolve_bidi_explicit(
        codepoints,
        direction,
        &units,
        stats,
        error);
    if (snapshot != nullptr) {
        *snapshot = ledger.snapshot(zevryon::core::ResourceClass::BidiRun);
    }
    if (result) {
        output->assign(units.begin(), units.end());
    }
    return result;
}

bool test_property_lookup() {
    BidiClass parsed = BidiClass::L;
    return require(zevryon::text::bidi_class_of('A') == BidiClass::L, "Latin class") &&
           require(zevryon::text::bidi_class_of(0x05d0U) == BidiClass::R, "Hebrew class") &&
           require(zevryon::text::bidi_class_of(0x0627U) == BidiClass::AL, "Arabic class") &&
           require(zevryon::text::bidi_class_of('1') == BidiClass::EN, "European number") &&
           require(zevryon::text::bidi_class_of(0x2066U) == BidiClass::LRI, "LRI class") &&
           require(zevryon::text::bidi_class_short_name(BidiClass::RLI) == "RLI", "short name") &&
           require(
               zevryon::text::bidi_class_long_name(BidiClass::RLI) == "Right_To_Left_Isolate",
               "long name") &&
           require(
               zevryon::text::bidi_class_from_name("Arabic_Letter", &parsed) &&
                   parsed == BidiClass::AL,
               "parse long class name");
}

bool test_paragraph_direction() {
    std::vector<BidiExplicitUnit> units;
    BidiExplicitStats stats;
    BidiExplicitError error;

    const auto latin = make_codepoints({'1', ' ', 'a'});
    if (!require(resolve(latin, BidiParagraphDirection::Auto, &units, &stats, &error), "Latin auto resolves") ||
        !require(stats.paragraph_level == 0U, "Latin paragraph level")) {
        return false;
    }

    const auto hebrew = make_codepoints({'1', ' ', 0x05d0U});
    if (!require(resolve(hebrew, BidiParagraphDirection::Auto, &units, &stats, &error), "Hebrew auto resolves") ||
        !require(stats.paragraph_level == 1U, "Hebrew paragraph level")) {
        return false;
    }

    const auto isolate_skipped = make_codepoints({0x2067U, 0x05d0U, 0x2069U, 'a'});
    return require(
               resolve(isolate_skipped, BidiParagraphDirection::Auto, &units, &stats, &error),
               "isolate paragraph resolves") &&
           require(stats.paragraph_level == 0U, "isolate contents skipped for paragraph level");
}

bool test_embeddings_and_overrides() {
    std::vector<BidiExplicitUnit> units;
    BidiExplicitStats stats;
    BidiExplicitError error;

    const auto embedded = make_codepoints({0x202aU, 'a', 0x202cU, 'b'});
    if (!require(resolve(embedded, BidiParagraphDirection::Left, &units, &stats, &error), "LRE resolves") ||
        !require(units.size() == 4U, "LRE unit count") ||
        !require(units[0].level == 0U, "LRE control level") ||
        !require((units[0].flags & zevryon::text::kBidiUnitRemovedByX9) != 0U, "LRE X9 flag") ||
        !require(units[1].level == 2U, "LRE content level") ||
        !require(units[3].level == 0U, "PDF restores level")) {
        return false;
    }

    const auto override = make_codepoints({0x202eU, '1', 0x202cU});
    return require(resolve(override, BidiParagraphDirection::Left, &units, &stats, &error), "RLO resolves") &&
           require(units[1].level == 1U, "RLO level") &&
           require(units[1].original_class == BidiClass::EN, "RLO original class") &&
           require(units[1].resolved_class == BidiClass::R, "RLO override class");
}

bool test_isolates_and_fsi() {
    std::vector<BidiExplicitUnit> units;
    BidiExplicitStats stats;
    BidiExplicitError error;

    const auto rli = make_codepoints({0x2067U, 0x05d0U, 0x2069U, 'a'});
    if (!require(resolve(rli, BidiParagraphDirection::Auto, &units, &stats, &error), "RLI resolves") ||
        !require(stats.paragraph_level == 0U, "RLI outer paragraph") ||
        !require(units[1].level == 1U, "RLI content level") ||
        !require(units[3].level == 0U, "PDI restores outer level") ||
        !require(stats.valid_isolates == 1U, "valid isolate counted")) {
        return false;
    }

    const auto fsi_right = make_codepoints({0x2068U, 0x05d0U, 0x2069U, 'a'});
    if (!require(resolve(fsi_right, BidiParagraphDirection::Auto, &units, &stats, &error), "FSI right resolves") ||
        !require(units[1].level == 1U, "FSI detects right") ||
        !require(stats.fsi_resolutions == 1U, "FSI counted")) {
        return false;
    }

    const auto fsi_left = make_codepoints({0x2068U, 'a', 0x2069U, 0x05d0U});
    return require(resolve(fsi_left, BidiParagraphDirection::Auto, &units, &stats, &error), "FSI left resolves") &&
           require(stats.paragraph_level == 1U, "FSI left outer right paragraph") &&
           require(units[1].level == 2U, "FSI detects left inside right paragraph");
}

bool test_overflow_and_unmatched_controls() {
    std::vector<std::uint32_t> values(140U, 0x202aU);
    values.push_back('a');
    std::vector<DecodedCodePoint> codepoints;
    codepoints.reserve(values.size());
    std::uint64_t source = 0U;
    for (const std::uint32_t value : values) {
        const std::uint8_t length = utf8_length(value);
        codepoints.emplace_back(value, source, source + length, false);
        source += length;
    }

    std::vector<BidiExplicitUnit> units;
    BidiExplicitStats stats;
    BidiExplicitError error;
    if (!require(resolve(codepoints, BidiParagraphDirection::Left, &units, &stats, &error), "overflow input resolves") ||
        !require(stats.overflow_embeddings > 0U, "embedding overflow counted") ||
        !require(stats.maximum_level <= 125U, "maximum level bounded")) {
        return false;
    }

    const auto unmatched = make_codepoints({0x202cU, 0x2069U, 'a'});
    return require(resolve(unmatched, BidiParagraphDirection::Left, &units, &stats, &error), "unmatched controls resolve") &&
           require(stats.unmatched_pdf == 1U, "unmatched PDF counted") &&
           require(stats.unmatched_pdi == 1U, "unmatched PDI counted");
}

bool test_invalid_input_and_budget() {
    auto invalid = make_codepoints({'a', 'b'});
    invalid[1].source_start += 1U;
    std::vector<BidiExplicitUnit> units;
    BidiExplicitStats stats;
    BidiExplicitError error;
    if (!require(
            !resolve(invalid, BidiParagraphDirection::Auto, &units, &stats, &error),
            "discontinuous input rejected") ||
        !require(error.kind == BidiExplicitErrorKind::InvalidInput, "invalid input error kind")) {
        return false;
    }

    const auto valid = make_codepoints({'a', 0x05d0U, 0x0627U});
    zevryon::core::ResourceSnapshot snapshot;
    return require(
               !resolve(
                   valid,
                   BidiParagraphDirection::Auto,
                   &units,
                   &stats,
                   &error,
                   1U,
                   &snapshot),
               "hard budget rejects output") &&
           require(error.kind == BidiExplicitErrorKind::OutputBudgetExceeded, "budget error kind") &&
           require(snapshot.rejected_reservations > 0U, "budget rejection counted") &&
           require(snapshot.current_bytes == 0U, "failed output releases temporary state");
}

} // namespace

int main() {
    if (!test_property_lookup() ||
        !test_paragraph_direction() ||
        !test_embeddings_and_overrides() ||
        !test_isolates_and_fsi() ||
        !test_overflow_and_unmatched_controls() ||
        !test_invalid_input_and_budget()) {
        return 1;
    }
    std::cout << "bidi explicit tests passed\n";
    return 0;
}
