#include "font_catalog.hpp"
#include "font_fallback.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>

namespace {

using zevryon::core::LedgerMemoryResource;
using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;
using namespace zevryon::text;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool neutral_style_can_beat_exact_script() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::FontCatalog, 16384U);
    ledger.set_hard_limit(ResourceClass::FontFallbackPlan, 4096U);
    LedgerMemoryResource catalog_resource(ledger, ResourceClass::FontCatalog);
    LedgerMemoryResource fallback_resource(ledger, ResourceClass::FontFallbackPlan);

    const ScriptId greek = [] {
        ScriptId value = ScriptId::Zzzz;
        script_id_from_name("Grek", &value);
        return value;
    }();
    const std::array<FontCoverageRange, 1> coverage{{{0x03b1U, 0x03b1U}}};
    const std::array<FontFaceSeed, 3> seeds{{
        {10U, 1U, 400U, 5U, FontSlant::Italic, greek, 0U, coverage},
        {20U, 2U, 400U, 5U, FontSlant::Upright, ScriptId::Zyyy, 0U, coverage},
        {30U, 3U, 400U, 5U, FontSlant::Upright, ScriptId::Latn, 0U, coverage},
    }};

    FontCatalog catalog(&catalog_resource);
    FontCatalogStats catalog_stats;
    FontCatalogError catalog_error;
    if (!build_font_catalog(seeds, &catalog, &catalog_stats, &catalog_error)) {
        return expect(false, "fixture catalog must build");
    }

    const std::array<DecodedCodePoint, 1> codepoints{{
        DecodedCodePoint{0x03b1U, 0U, 2U, false},
    }};
    const std::array<GraphemeBoundary, 2> graphemes{{
        {0U, 0U},
        {2U, 1U},
    }};
    const std::array<ScriptRunBoundary, 2> script_runs{{
        {0U, 0U, greek, 0U},
        {2U, 1U, greek, 0U},
    }};
    const FontFallbackRequest request{
        kInvalidFontFaceId,
        {},
        {400U, 5U, FontSlant::Upright},
    };

    FontFallbackPlan plan(&fallback_resource);
    FontFallbackStats stats;
    FontFallbackError error;
    bool ok = build_font_fallback_plan(
        codepoints,
        graphemes,
        script_runs,
        catalog,
        request,
        &plan,
        &stats,
        &error);
    ok &= expect(plan.boundaries.size() == 2U, "one run plus sentinel is required");
    const FontFaceId neutral = font_face_id_by_stable_key(catalog, 20U);
    ok &= expect(plan.boundaries[0].face_id == neutral,
                 "neutral upright face must beat exact-script italic face by full score");
    ok &= expect(plan.boundaries[0].source == FontFallbackSource::NeutralScript,
                 "winning neutral face must keep its source classification");
    return ok;
}

bool corrupt_script_index_is_rejected() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::FontCatalog, 16384U);
    ledger.set_hard_limit(ResourceClass::FontFallbackPlan, 4096U);
    LedgerMemoryResource catalog_resource(ledger, ResourceClass::FontCatalog);
    LedgerMemoryResource fallback_resource(ledger, ResourceClass::FontFallbackPlan);

    const ScriptId latin = [] {
        ScriptId value = ScriptId::Zzzz;
        script_id_from_name("Latn", &value);
        return value;
    }();
    const std::array<FontCoverageRange, 1> coverage{{{0x0061U, 0x0061U}}};
    const std::array<FontFaceSeed, 2> seeds{{
        {10U, 1U, 400U, 5U, FontSlant::Upright, latin, 0U, coverage},
        {20U, 2U, 400U, 5U, FontSlant::Upright, latin, 0U, coverage},
    }};

    FontCatalog catalog(&catalog_resource);
    FontCatalogStats catalog_stats;
    FontCatalogError catalog_error;
    if (!build_font_catalog(seeds, &catalog, &catalog_stats, &catalog_error)) {
        return expect(false, "corruption fixture catalog must build");
    }
    catalog.script_face_ids[1] = catalog.script_face_ids[0];

    const std::array<DecodedCodePoint, 1> codepoints{{
        DecodedCodePoint{0x0061U, 0U, 1U, false},
    }};
    const std::array<GraphemeBoundary, 2> graphemes{{{0U, 0U}, {1U, 1U}}};
    const std::array<ScriptRunBoundary, 2> script_runs{{
        {0U, 0U, latin, 0U},
        {1U, 1U, latin, 0U},
    }};
    const FontFallbackRequest request{
        kInvalidFontFaceId,
        {},
        {400U, 5U, FontSlant::Upright},
    };
    FontFallbackPlan plan(&fallback_resource);
    FontFallbackStats stats;
    FontFallbackError error;
    const bool built = build_font_fallback_plan(
        codepoints,
        graphemes,
        script_runs,
        catalog,
        request,
        &plan,
        &stats,
        &error);
    bool ok = expect(!built, "corrupt script index must fail closed");
    ok &= expect(error.kind == FontFallbackErrorKind::InvalidInput,
                 "corrupt script index must report invalid input");
    ok &= expect(plan.boundaries.empty(), "corrupt index must publish no plan");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= neutral_style_can_beat_exact_script();
    ok &= corrupt_script_index_is_rejected();
    if (!ok) {
        return 1;
    }
    std::cout << "font fallback bucket equivalence tests passed\n";
    return 0;
}
