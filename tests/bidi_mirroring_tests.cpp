#include "bidi_mirroring.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_bidi_mirroring.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

using zevryon::text::BidiClass;
using zevryon::text::BidiExplicitUnit;
using zevryon::text::BidiMirrorError;
using zevryon::text::BidiMirrorErrorKind;
using zevryon::text::BidiMirrorKind;
using zevryon::text::BidiMirrorRequests;
using zevryon::text::BidiMirrorStats;
using zevryon::text::BidiSequenceTopology;
using zevryon::text::BidiVisualOrder;
using zevryon::text::DecodedCodePoint;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

struct CaseData {
    std::vector<DecodedCodePoint> codepoints;
    std::vector<BidiExplicitUnit> units;
    BidiSequenceTopology topology;
    BidiVisualOrder visual;

    CaseData()
        : topology(std::pmr::get_default_resource()),
          visual(std::pmr::get_default_resource()) {}
};

CaseData make_case(
    const std::vector<std::uint32_t>& values,
    const std::vector<std::uint8_t>& levels,
    const std::vector<std::uint32_t>& visual_to_active) {
    CaseData data;
    for (std::size_t index = 0U; index < values.size(); ++index) {
        data.codepoints.emplace_back(
            values[index],
            static_cast<std::uint64_t>(index),
            static_cast<std::uint64_t>(index + 1U),
            false);
        data.units.push_back(BidiExplicitUnit{
            static_cast<std::uint64_t>(index),
            static_cast<std::uint32_t>(index),
            BidiClass::ON,
            BidiClass::ON,
            levels[index],
            0U});
        data.topology.active_unit_indices.push_back(
            static_cast<std::uint32_t>(index));
    }
    data.visual.line_levels.assign(levels.begin(), levels.end());
    data.visual.visual_to_active.assign(
        visual_to_active.begin(), visual_to_active.end());
    return data;
}

bool resolve_case(
    CaseData& data,
    std::size_t budget,
    BidiMirrorRequests* output,
    BidiMirrorStats* stats,
    BidiMirrorError* error,
    zevryon::core::ResourceLedger* ledger) {
    ledger->set_hard_limit(
        zevryon::core::ResourceClass::BidiMirrorRequest,
        budget);
    return zevryon::text::build_bidi_mirror_requests(
        data.codepoints,
        data.units,
        data.topology,
        data.visual,
        output,
        stats,
        error);
}

bool test_unicode_property_and_mapping_modes() {
    const auto exact = zevryon::text::bidi_mirroring_info(0x28U);
    const auto best_fit = zevryon::text::bidi_mirroring_info(0x2209U);
    const auto glyph_only = zevryon::text::bidi_mirroring_info(0x221BU);
    const auto legacy = zevryon::text::bidi_mirroring_info(0xFD3EU);
    return require(
               exact.mirrored && exact.has_character_mapping &&
                   !exact.best_fit && exact.mirror_codepoint == 0x29U,
               "left parenthesis has an exact mirror mapping") &&
           require(
               best_fit.mirrored && best_fit.has_character_mapping &&
                   best_fit.best_fit && best_fit.mirror_codepoint == 0x220CU,
               "not-an-element-of has a best-fit mapping") &&
           require(
               glyph_only.mirrored && !glyph_only.has_character_mapping &&
                   glyph_only.mirror_codepoint == 0U,
               "cube root requires mirrored glyph handling") &&
           require(
               !legacy.mirrored && !legacy.has_character_mapping,
               "ornate parenthesis legacy exception is not mirrored");
}

bool test_sparse_visual_order_requests() {
    CaseData data = make_case(
        {0x28U, 0x41U, 0x2209U, 0x221BU, 0xFD3EU},
        {1U, 0U, 1U, 1U, 1U},
        {3U, 2U, 4U, 1U, 0U});
    zevryon::core::ResourceLedger ledger;
    zevryon::core::LedgerMemoryResource memory(
        ledger,
        zevryon::core::ResourceClass::BidiMirrorRequest);
    BidiMirrorRequests output(&memory);
    BidiMirrorStats stats;
    BidiMirrorError error;
    if (!require(
            resolve_case(
                data,
                4096U,
                &output,
                &stats,
                &error,
                &ledger),
            "mixed L4 case resolves") ||
        !require(output.requests.size() == 3U, "only mirrored odd-level units emit") ||
        !require(
            output.requests[0].visual_index == 0U &&
                output.requests[0].mirror_codepoint == 0U &&
                output.requests[0].kind == BidiMirrorKind::MirroredGlyphOnly,
            "glyph-only request preserves visual position") ||
        !require(
            output.requests[1].visual_index == 1U &&
                output.requests[1].mirror_codepoint == 0x220CU &&
                output.requests[1].kind == BidiMirrorKind::BestFitCharacter,
            "best-fit request preserves visual position") ||
        !require(
            output.requests[2].visual_index == 4U &&
                output.requests[2].mirror_codepoint == 0x29U &&
                output.requests[2].kind == BidiMirrorKind::ExactCharacter,
            "exact request preserves visual position")) {
        return false;
    }

    const auto resource = ledger.snapshot(
        zevryon::core::ResourceClass::BidiMirrorRequest);
    return require(stats.odd_level_units == 4U, "odd-level counter") &&
           require(stats.mirrored_property_hits == 3U, "mirrored-property counter") &&
           require(stats.exact_character_requests == 1U, "exact counter") &&
           require(stats.best_fit_character_requests == 1U, "best-fit counter") &&
           require(stats.glyph_only_requests == 1U, "glyph-only counter") &&
           require(stats.output_requests == 3U, "output counter") &&
           require(resource.current_bytes == 36U, "three requests cost exactly 36 bytes") &&
           require(resource.peak_bytes == 36U, "exact reserve prevents growth overhead") &&
           require(resource.accounting_errors == 0U, "request accounting is clean");
}

bool test_even_level_and_fail_closed_contracts() {
    CaseData even = make_case({0x28U}, {0U}, {0U});
    zevryon::core::ResourceLedger even_ledger;
    zevryon::core::LedgerMemoryResource even_memory(
        even_ledger,
        zevryon::core::ResourceClass::BidiMirrorRequest);
    BidiMirrorRequests even_output(&even_memory);
    BidiMirrorStats even_stats;
    BidiMirrorError even_error;
    if (!require(
            resolve_case(
                even,
                1U,
                &even_output,
                &even_stats,
                &even_error,
                &even_ledger),
            "even-level mirrored character resolves without allocation") ||
        !require(even_output.requests.empty(), "even level suppresses L4 request") ||
        !require(
            even_ledger.snapshot(
                zevryon::core::ResourceClass::BidiMirrorRequest)
                    .peak_bytes == 0U,
            "empty sparse output allocates nothing")) {
        return false;
    }

    CaseData invalid = make_case({0x28U}, {1U}, {0U});
    invalid.visual.line_levels.clear();
    zevryon::core::ResourceLedger invalid_ledger;
    zevryon::core::LedgerMemoryResource invalid_memory(
        invalid_ledger,
        zevryon::core::ResourceClass::BidiMirrorRequest);
    BidiMirrorRequests invalid_output(&invalid_memory);
    invalid_output.requests.push_back({9U, 9U, BidiMirrorKind::ExactCharacter, 0U, 0U});
    BidiMirrorStats invalid_stats;
    BidiMirrorError invalid_error;
    if (!require(
            !resolve_case(
                invalid,
                4096U,
                &invalid_output,
                &invalid_stats,
                &invalid_error,
                &invalid_ledger),
            "stage-size mismatch is rejected") ||
        !require(
            invalid_error.kind == BidiMirrorErrorKind::InvalidInput,
            "invalid stage failure kind") ||
        !require(
            invalid_output.requests.empty(),
            "invalid stage publishes no stale output")) {
        return false;
    }

    CaseData budget = make_case({0x28U}, {1U}, {0U});
    zevryon::core::ResourceLedger budget_ledger;
    zevryon::core::LedgerMemoryResource budget_memory(
        budget_ledger,
        zevryon::core::ResourceClass::BidiMirrorRequest);
    BidiMirrorRequests budget_output(&budget_memory);
    BidiMirrorStats budget_stats;
    BidiMirrorError budget_error;
    return require(
               !resolve_case(
                   budget,
                   sizeof(zevryon::text::BidiMirrorRequest) - 1U,
                   &budget_output,
                   &budget_stats,
                   &budget_error,
                   &budget_ledger),
               "undersized mirror budget is rejected") &&
           require(
               budget_error.kind == BidiMirrorErrorKind::OutputBudgetExceeded,
               "budget failure kind") &&
           require(budget_output.requests.empty(), "budget failure publishes no output") &&
           require(
               budget_ledger.snapshot(
                   zevryon::core::ResourceClass::BidiMirrorRequest)
                       .rejected_reservations > 0U,
               "budget rejection is accounted");
}

} // namespace

int main() {
    if (!test_unicode_property_and_mapping_modes() ||
        !test_sparse_visual_order_requests() ||
        !test_even_level_and_fail_closed_contracts()) {
        return 1;
    }
    std::cout << "Bidi mirroring tests passed\n";
    return 0;
}
