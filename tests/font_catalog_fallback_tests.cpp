#include "font_catalog.hpp"
#include "font_fallback.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using zevryon::core::LedgerMemoryResource;
using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;
using namespace zevryon::text;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

ScriptId require_script(std::string_view name) {
    ScriptId script = ScriptId::Zzzz;
    if (!script_id_from_name(name, &script)) {
        std::cerr << "required script not found: " << name << '\n';
    }
    return script;
}

std::vector<DecodedCodePoint> make_codepoints(
    std::initializer_list<std::uint32_t> values) {
    std::vector<DecodedCodePoint> output;
    std::uint64_t offset = 0U;
    for (const std::uint32_t value : values) {
        output.emplace_back(value, offset, offset + 1U, false);
        ++offset;
    }
    return output;
}

bool test_catalog_canonicalization() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::FontCatalog, 4096U);
    LedgerMemoryResource resource(ledger, ResourceClass::FontCatalog);
    FontCatalog catalog(&resource);

    const ScriptId latn = require_script("Latn");
    const ScriptId arab = require_script("Arab");
    const std::array<FontCoverageRange, 2> latin_ranges{{
        {0x0020U, 0x007eU},
        {0x007fU, 0x00ffU},
    }};
    const std::array<FontCoverageRange, 1> arabic_ranges{{
        {0x0600U, 0x06ffU},
    }};
    const std::array<FontFaceSeed, 2> seeds{{
        {20U, 2U, 500U, 5U, FontSlant::Italic, arab, kFontFaceSystem, arabic_ranges},
        {10U, 1U, 400U, 5U, FontSlant::Upright, latn, kFontFaceVariable, latin_ranges},
    }};

    FontCatalogStats stats;
    FontCatalogError error;
    bool ok = build_font_catalog(seeds, &catalog, &stats, &error);
    ok &= expect(error.kind == FontCatalogErrorKind::None, "catalog error must be clear");
    ok &= expect(catalog.faces.size() == 2U, "catalog must contain two faces");
    ok &= expect(catalog.faces[0].stable_key == 10U, "faces must be stable-key sorted");
    ok &= expect(catalog.faces[1].stable_key == 20U, "second stable key must be preserved");
    ok &= expect(catalog.coverage_ranges.size() == 2U, "adjacent Latin ranges must merge");
    ok &= expect(stats.adjacent_ranges_merged == 1U, "one adjacent range must be merged");
    ok &= expect(font_face_id_by_stable_key(catalog, 10U) == 0U, "stable-key lookup must resolve");
    ok &= expect(font_face_covers(catalog, 0U, 0x0041U), "Latin face must cover A");
    ok &= expect(!font_face_covers(catalog, 0U, 0x0301U), "Latin fixture must not cover combining acute");
    ok &= expect(ledger.accounting_clean(), "catalog accounting must remain clean");
    return ok;
}

bool test_catalog_rejections() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::FontCatalog, 4096U);
    LedgerMemoryResource resource(ledger, ResourceClass::FontCatalog);
    FontCatalog catalog(&resource);
    const ScriptId latn = require_script("Latn");
    const std::array<FontCoverageRange, 1> coverage{{{0x0020U, 0x007eU}}};
    const std::array<FontFaceSeed, 2> duplicates{{
        {7U, 1U, 400U, 5U, FontSlant::Upright, latn, 0U, coverage},
        {7U, 2U, 700U, 5U, FontSlant::Italic, latn, 0U, coverage},
    }};
    FontCatalogStats stats;
    FontCatalogError error;
    bool ok = expect(
        !build_font_catalog(duplicates, &catalog, &stats, &error),
        "duplicate stable keys must fail");
    ok &= expect(error.kind == FontCatalogErrorKind::DuplicateStableKey, "duplicate error kind must be exact");
    ok &= expect(catalog.faces.empty() && catalog.coverage_ranges.empty(), "failed catalog must publish nothing");

    ResourceLedger tiny_ledger;
    tiny_ledger.set_hard_limit(ResourceClass::FontCatalog, 1U);
    LedgerMemoryResource tiny_resource(tiny_ledger, ResourceClass::FontCatalog);
    FontCatalog tiny_catalog(&tiny_resource);
    const std::array<FontFaceSeed, 1> one_face{{
        {9U, 1U, 400U, 5U, FontSlant::Upright, latn, 0U, coverage},
    }};
    error = {};
    stats = {};
    ok &= expect(
        !build_font_catalog(one_face, &tiny_catalog, &stats, &error),
        "catalog hard budget must fail");
    ok &= expect(error.kind == FontCatalogErrorKind::OutputBudgetExceeded, "catalog budget error must be exact");
    ok &= expect(tiny_catalog.faces.empty(), "budget failure must publish no faces");
    ok &= expect(tiny_ledger.snapshot(ResourceClass::FontCatalog).rejected_reservations > 0U,
                 "catalog budget rejection must be accounted");
    return ok;
}

bool build_fixture_catalog(
    FontCatalog* catalog,
    FontCatalogStats* stats,
    FontCatalogError* error,
    FontFaceId* latin_primary,
    FontFaceId* latin_complete,
    FontFaceId* greek,
    FontFaceId* neutral) {
    static const std::array<FontCoverageRange, 1> primary_ranges{{
        {0x0061U, 0x0061U},
    }};
    static const std::array<FontCoverageRange, 2> complete_ranges{{
        {0x0061U, 0x0061U},
        {0x0301U, 0x0301U},
    }};
    static const std::array<FontCoverageRange, 1> greek_ranges{{
        {0x0370U, 0x03ffU},
    }};
    static const std::array<FontCoverageRange, 3> neutral_ranges{{
        {0x0061U, 0x0061U},
        {0x0301U, 0x0301U},
        {0x0370U, 0x03ffU},
    }};
    const ScriptId latn = require_script("Latn");
    const ScriptId grek = require_script("Grek");
    static std::array<FontFaceSeed, 4> seeds;
    seeds = {{
        {10U, 1U, 400U, 5U, FontSlant::Upright, latn, 0U, primary_ranges},
        {20U, 2U, 400U, 5U, FontSlant::Upright, latn, 0U, complete_ranges},
        {30U, 3U, 700U, 5U, FontSlant::Upright, grek, 0U, greek_ranges},
        {40U, 4U, 400U, 5U, FontSlant::Upright, ScriptId::Zyyy, 0U, neutral_ranges},
    }};
    if (!build_font_catalog(seeds, catalog, stats, error)) {
        return false;
    }
    *latin_primary = font_face_id_by_stable_key(*catalog, 10U);
    *latin_complete = font_face_id_by_stable_key(*catalog, 20U);
    *greek = font_face_id_by_stable_key(*catalog, 30U);
    *neutral = font_face_id_by_stable_key(*catalog, 40U);
    return true;
}

bool test_grapheme_atomic_preferred_fallback() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::FontCatalog, 8192U);
    ledger.set_hard_limit(ResourceClass::FontFallbackPlan, 4096U);
    LedgerMemoryResource catalog_resource(ledger, ResourceClass::FontCatalog);
    LedgerMemoryResource fallback_resource(ledger, ResourceClass::FontFallbackPlan);
    FontCatalog catalog(&catalog_resource);
    FontCatalogStats catalog_stats;
    FontCatalogError catalog_error;
    FontFaceId primary = kInvalidFontFaceId;
    FontFaceId complete = kInvalidFontFaceId;
    FontFaceId greek = kInvalidFontFaceId;
    FontFaceId neutral = kInvalidFontFaceId;
    if (!build_fixture_catalog(
            &catalog,
            &catalog_stats,
            &catalog_error,
            &primary,
            &complete,
            &greek,
            &neutral)) {
        return expect(false, "fixture catalog must build");
    }

    const std::vector<DecodedCodePoint> codepoints = make_codepoints({0x0061U, 0x0301U});
    const std::array<GraphemeBoundary, 2> graphemes{{
        {0U, 0U},
        {2U, 2U},
    }};
    const ScriptId latn = require_script("Latn");
    const std::array<ScriptRunBoundary, 2> scripts{{
        {0U, 0U, latn, 0U},
        {2U, 1U, latn, 0U},
    }};
    const std::array<FontFaceId, 1> preferred{{complete}};
    const FontFallbackRequest request{
        primary,
        preferred,
        {400U, 5U, FontSlant::Upright},
    };
    FontFallbackPlan plan(&fallback_resource);
    FontFallbackStats stats;
    FontFallbackError error;
    bool ok = build_font_fallback_plan(
        codepoints,
        graphemes,
        scripts,
        catalog,
        request,
        &plan,
        &stats,
        &error);
    ok &= expect(plan.boundaries.size() == 2U, "one fallback run plus sentinel is required");
    ok &= expect(plan.boundaries[0].face_id == complete, "complete grapheme font must win");
    ok &= expect(plan.boundaries[0].source == FontFallbackSource::PreferredFamily,
                 "preferred-family source must be recorded");
    ok &= expect(stats.primary_clusters == 0U, "partial primary coverage must not split a grapheme");
    ok &= expect(stats.preferred_family_clusters == 1U, "preferred cluster count must be one");
    ok &= expect(stats.missing_clusters == 0U, "covered grapheme must not be missing");
    ok &= expect(plan.boundaries.back().cluster_index == 1U, "sentinel must equal cluster count");
    return ok;
}

bool test_script_ranking_and_missing() {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::FontCatalog, 8192U);
    ledger.set_hard_limit(ResourceClass::FontFallbackPlan, 4096U);
    LedgerMemoryResource catalog_resource(ledger, ResourceClass::FontCatalog);
    LedgerMemoryResource fallback_resource(ledger, ResourceClass::FontFallbackPlan);
    FontCatalog catalog(&catalog_resource);
    FontCatalogStats catalog_stats;
    FontCatalogError catalog_error;
    FontFaceId primary = kInvalidFontFaceId;
    FontFaceId complete = kInvalidFontFaceId;
    FontFaceId greek = kInvalidFontFaceId;
    FontFaceId neutral = kInvalidFontFaceId;
    if (!build_fixture_catalog(
            &catalog,
            &catalog_stats,
            &catalog_error,
            &primary,
            &complete,
            &greek,
            &neutral)) {
        return expect(false, "fixture catalog must build");
    }

    const std::vector<DecodedCodePoint> codepoints = make_codepoints({0x03b1U, 0x4e00U});
    const std::array<GraphemeBoundary, 3> graphemes{{
        {0U, 0U},
        {1U, 1U},
        {2U, 2U},
    }};
    const ScriptId grek = require_script("Grek");
    const ScriptId hani = require_script("Hani");
    const std::array<ScriptRunBoundary, 3> scripts{{
        {0U, 0U, grek, 0U},
        {1U, 1U, hani, 0U},
        {2U, 2U, hani, 0U},
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
        scripts,
        catalog,
        request,
        &plan,
        &stats,
        &error);
    ok &= expect(plan.boundaries.size() == 3U, "Greek and missing clusters require two runs plus sentinel");
    ok &= expect(plan.boundaries[0].face_id == greek, "script match must beat closer neutral style");
    ok &= expect(plan.boundaries[0].source == FontFallbackSource::ScriptMatch,
                 "Greek run must be script matched");
    ok &= expect(plan.boundaries[1].face_id == kInvalidFontFaceId, "uncovered Han cluster must be missing");
    ok &= expect(plan.boundaries[1].source == FontFallbackSource::Missing,
                 "missing source must be explicit");
    ok &= expect(stats.script_match_clusters == 1U, "one script match is expected");
    ok &= expect(stats.missing_clusters == 1U, "one missing cluster is expected");
    return ok;
}

bool test_fallback_budget_failure_is_atomic() {
    ResourceLedger catalog_ledger;
    catalog_ledger.set_hard_limit(ResourceClass::FontCatalog, 8192U);
    LedgerMemoryResource catalog_resource(catalog_ledger, ResourceClass::FontCatalog);
    FontCatalog catalog(&catalog_resource);
    FontCatalogStats catalog_stats;
    FontCatalogError catalog_error;
    FontFaceId primary = kInvalidFontFaceId;
    FontFaceId complete = kInvalidFontFaceId;
    FontFaceId greek = kInvalidFontFaceId;
    FontFaceId neutral = kInvalidFontFaceId;
    if (!build_fixture_catalog(
            &catalog,
            &catalog_stats,
            &catalog_error,
            &primary,
            &complete,
            &greek,
            &neutral)) {
        return expect(false, "fixture catalog must build");
    }

    ResourceLedger tiny_ledger;
    tiny_ledger.set_hard_limit(ResourceClass::FontFallbackPlan, 1U);
    LedgerMemoryResource tiny_resource(tiny_ledger, ResourceClass::FontFallbackPlan);
    FontFallbackPlan plan(&tiny_resource);
    const std::vector<DecodedCodePoint> codepoints = make_codepoints({0x0061U});
    const std::array<GraphemeBoundary, 2> graphemes{{{0U, 0U}, {1U, 1U}}};
    const ScriptId latn = require_script("Latn");
    const std::array<ScriptRunBoundary, 2> scripts{{
        {0U, 0U, latn, 0U},
        {1U, 1U, latn, 0U},
    }};
    const FontFallbackRequest request{primary, {}, {400U, 5U, FontSlant::Upright}};
    FontFallbackStats stats;
    FontFallbackError error;
    bool ok = expect(
        !build_font_fallback_plan(
            codepoints,
            graphemes,
            scripts,
            catalog,
            request,
            &plan,
            &stats,
            &error),
        "fallback hard budget must fail");
    ok &= expect(error.kind == FontFallbackErrorKind::OutputBudgetExceeded,
                 "fallback budget error must be exact");
    ok &= expect(plan.boundaries.empty(), "budget failure must publish no boundaries");
    ok &= expect(stats.output_runs == 0U, "budget failure must publish no stats");
    ok &= expect(tiny_ledger.snapshot(ResourceClass::FontFallbackPlan).rejected_reservations > 0U,
                 "fallback budget rejection must be accounted");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_catalog_canonicalization();
    ok &= test_catalog_rejections();
    ok &= test_grapheme_atomic_preferred_fallback();
    ok &= test_script_ranking_and_missing();
    ok &= test_fallback_budget_failure_is_atomic();
    if (!ok) {
        return 1;
    }
    std::cout << "font catalog and fallback tests passed\n";
    return 0;
}
