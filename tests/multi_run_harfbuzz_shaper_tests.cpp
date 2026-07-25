#include "ledger_memory_resource.hpp"
#include "multi_run_harfbuzz_shaper.hpp"
#include "resource_ledger.hpp"
#include "unicode_script.hpp"
#include "unicode_stream.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {
using namespace zevryon::core;
using namespace zevryon::text;

bool expect(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return value;
}

void append_identity_field(std::string* output, std::string_view value) {
    output->append(std::to_string(value.size()));
    output->push_back(':');
    output->append(value);
    output->push_back('|');
}

std::string utf8_path(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

ScriptId script(std::string_view name) {
    ScriptId value = ScriptId::Zzzz;
    (void)script_id_from_name(name, &value);
    return value;
}

std::vector<ShapedGlyph> copy_glyphs(const ShapedGlyphRun& run) {
    return {run.glyphs.begin(), run.glyphs.end()};
}

struct Fixture {
    std::pmr::monotonic_buffer_resource text_memory;
    std::pmr::vector<DecodedCodePoint> codepoints{&text_memory};
    std::pmr::vector<GraphemeBoundary> graphemes{&text_memory};
    std::array<std::string, 2U> identities;
    std::array<std::string, 2U> families{{"Multi Latin", "Multi Arabic"}};
    std::array<FontCoverageRange, 2U> coverage{{
        {0x20U, 0x7eU},
        {0x0600U, 0x06ffU},
    }};
    std::array<FontDiscoveryFace, 2U> faces;
    std::shared_ptr<const FontCatalogGeneration> generation;
    std::array<CatalogFontFaceBinding, 2U> bindings;
    ShapingRunPlan plan;
    std::uint32_t split_cluster{0};

    bool build(
        const std::filesystem::path& font_path,
        std::size_t font_size,
        std::uint64_t generation_id,
        VerifiedFontResourceCache* resource_cache) {
        constexpr std::string_view text =
            "office "
            "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";
        Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
        Utf8DecodeError decode_error;
        const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
        if (!decoder.feed(bytes, 0U, &codepoints, &decode_error) ||
            !decoder.finish(&codepoints, &decode_error)) {
            return false;
        }
        GraphemeSegmentStats grapheme_stats;
        GraphemeError grapheme_error;
        if (!segment_graphemes(
                codepoints,
                &graphemes,
                &grapheme_stats,
                &grapheme_error)) {
            return false;
        }
        split_cluster = 7U;
        const std::uint32_t cluster_count =
            static_cast<std::uint32_t>(graphemes.size() - 1U);
        if (cluster_count <= split_cluster) {
            return false;
        }

        const std::string path = utf8_path(font_path);
        for (std::size_t index = 0U; index < identities.size(); ++index) {
            identities[index] = "fontconfig|";
            append_identity_field(&identities[index], "");
            append_identity_field(&identities[index], path);
            append_identity_field(&identities[index], "0");
            append_identity_field(
                &identities[index],
                index == 0U ? "ZevryonMultiA" : "ZevryonMultiB");
            append_identity_field(&identities[index], "");
        }
        faces[0] = FontDiscoveryFace{
            identities[0],
            families[0],
            400U,
            5U,
            FontSlant::Upright,
            script("Latn"),
            0U,
            coverage};
        faces[1] = FontDiscoveryFace{
            identities[1],
            families[1],
            400U,
            5U,
            FontSlant::Upright,
            script("Arab"),
            0U,
            coverage};

        FontDiscoveryStats discovery_stats;
        FontDiscoveryError discovery_error;
        if (!build_font_catalog_generation(
                generation_id,
                faces,
                2U * 1024U * 1024U,
                512U * 1024U,
                &generation,
                &discovery_stats,
                &discovery_error)) {
            return false;
        }
        for (FontFaceId face_id = 0U; face_id < bindings.size(); ++face_id) {
            CatalogFontResourceStats resource_stats;
            CatalogFontResourceError resource_error;
            if (!bind_catalog_font_face(
                    generation,
                    face_id,
                    font_size * 2U,
                    resource_cache,
                    &bindings[face_id],
                    &resource_stats,
                    &resource_error)) {
                return false;
            }
        }

        plan.boundaries.push_back(ShapingRunBoundary{
            0U,
            0U,
            script("Latn"),
            ShapingDirection::LeftToRight,
            FontFallbackSource::Primary,
            0U,
            0U});
        plan.boundaries.push_back(ShapingRunBoundary{
            split_cluster,
            1U,
            script("Arab"),
            ShapingDirection::RightToLeft,
            FontFallbackSource::ScriptMatch,
            1U,
            0U});
        plan.boundaries.push_back(ShapingRunBoundary{
            cluster_count,
            kInvalidFontFaceId,
            ScriptId::Zzzz,
            ShapingDirection::LeftToRight,
            FontFallbackSource::Missing,
            0U,
            0U});
        return true;
    }

    MultiRunCatalogHarfBuzzShapingRequest request(
        PreparedHarfBuzzFaceCache* cache) const {
        MultiRunCatalogHarfBuzzShapingRequest value;
        value.plan = &plan;
        value.bindings = bindings;
        value.prepared_face_cache = cache;
        value.codepoints = codepoints;
        value.grapheme_boundaries = graphemes;
        value.language = "und";
        return value;
    }
};

bool test_success_repeat_and_direct_equivalence(
    const Fixture& fixture,
    std::size_t font_size) {
    PreparedHarfBuzzFaceCache face_cache(
        font_size * 6U,
        256U * 1024U,
        4U);
    ResourceLedger metadata_ledger;
    ResourceLedger glyph_ledger;
    metadata_ledger.set_hard_limit(
        ResourceClass::MultiRunShapeMetadata,
        256U * 1024U);
    glyph_ledger.set_hard_limit(ResourceClass::GlyphRun, 2U * 1024U * 1024U);
    LedgerMemoryResource metadata_memory(
        metadata_ledger,
        ResourceClass::MultiRunShapeMetadata);
    LedgerMemoryResource glyph_memory(glyph_ledger, ResourceClass::GlyphRun);
    MultiRunShapedText output(&metadata_memory, &glyph_memory);
    MultiRunCatalogHarfBuzzShapingStats stats;
    MultiRunCatalogHarfBuzzShapingError error;

    bool ok = expect(
        shape_multi_run_catalog_harfbuzz(
            fixture.request(&face_cache),
            &output,
            &stats,
            &error),
        "two-run catalog shaping succeeds");
    ok &= expect(output.segments.size() == 2U, "two exact output segments");
    ok &= expect(
        stats.input_runs == 2U && stats.completed_runs == 2U &&
            stats.left_to_right_runs == 1U &&
            stats.right_to_left_runs == 1U &&
            stats.distinct_bound_faces == 2U,
        "multi-run statistics are exact");
    ok &= expect(
        stats.cache_after.preparation_attempts == 2U &&
            stats.cache_after.faces_published == 2U &&
            stats.cache_after.entry_count == 2U,
        "both immutable faces prepared once");
    ok &= expect(
        !output.segments[0].glyphs.glyphs.empty() &&
            !output.segments[1].glyphs.glyphs.empty(),
        "both logical runs publish glyphs");
    ok &= expect(
        output.segments[0].glyphs.first_cluster == 0U &&
            output.segments[0].glyphs.cluster_limit == fixture.split_cluster &&
            output.segments[1].glyphs.first_cluster == fixture.split_cluster &&
            output.segments[1].glyphs.cluster_limit ==
                fixture.graphemes.size() - 1U,
        "segment cluster ranges remain global and contiguous");

    std::array<std::vector<ShapedGlyph>, 2U> first_output{
        copy_glyphs(output.segments[0].glyphs),
        copy_glyphs(output.segments[1].glyphs)};
    for (std::size_t index = 0U; index < output.segments.size(); ++index) {
        ResourceLedger direct_ledger;
        direct_ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
        LedgerMemoryResource direct_memory(direct_ledger, ResourceClass::GlyphRun);
        ShapedGlyphRun direct_output(&direct_memory);
        CachedCatalogHarfBuzzShapingRequest direct_request;
        direct_request.binding = &fixture.bindings[index];
        direct_request.prepared_face_cache = &face_cache;
        direct_request.codepoints = fixture.codepoints;
        direct_request.grapheme_boundaries = fixture.graphemes;
        direct_request.first_cluster = fixture.plan.boundaries[index].cluster_index;
        direct_request.cluster_limit =
            fixture.plan.boundaries[index + 1U].cluster_index;
        direct_request.script = fixture.plan.boundaries[index].script;
        direct_request.direction = fixture.plan.boundaries[index].direction;
        direct_request.language = "und";
        direct_request.beginning_of_text = index == 0U;
        direct_request.end_of_text = index + 1U == output.segments.size();
        CachedCatalogHarfBuzzShapingStats direct_stats;
        CachedCatalogHarfBuzzShapingError direct_error;
        ok &= expect(
            shape_cached_catalog_harfbuzz_segment(
                direct_request,
                &direct_output,
                &direct_stats,
                &direct_error) &&
                copy_glyphs(direct_output) == first_output[index],
            "multi-run segment is byte-exact with direct cached shaping");
        ok &= expect(direct_ledger.accounting_clean(), "direct ledger stays clean");
    }

    MultiRunCatalogHarfBuzzShapingStats repeat_stats;
    MultiRunCatalogHarfBuzzShapingError repeat_error;
    ok &= expect(
        shape_multi_run_catalog_harfbuzz(
            fixture.request(&face_cache),
            &output,
            &repeat_stats,
            &repeat_error),
        "resident multi-run repeat succeeds");
    ok &= expect(
        copy_glyphs(output.segments[0].glyphs) == first_output[0] &&
            copy_glyphs(output.segments[1].glyphs) == first_output[1],
        "resident repeat is byte-exact");
    ok &= expect(
        repeat_stats.cache_after.hits >= repeat_stats.cache_before.hits + 2U,
        "repeat observes two resident prepared-face hits");

    ShapingRunPlan alternating_plan;
    const std::uint32_t cluster_count = static_cast<std::uint32_t>(
        fixture.graphemes.size() - 1U);
    for (std::uint32_t cluster_index = 0U;
         cluster_index < cluster_count;
         ++cluster_index) {
        const bool right_to_left = cluster_index >= fixture.split_cluster;
        alternating_plan.boundaries.push_back(ShapingRunBoundary{
            cluster_index,
            static_cast<FontFaceId>(cluster_index & 1U),
            right_to_left ? script("Arab") : script("Latn"),
            right_to_left ? ShapingDirection::RightToLeft
                          : ShapingDirection::LeftToRight,
            right_to_left ? FontFallbackSource::ScriptMatch
                          : FontFallbackSource::Primary,
            static_cast<std::uint8_t>(right_to_left ? 1U : 0U),
            0U});
    }
    alternating_plan.boundaries.push_back(ShapingRunBoundary{
        cluster_count,
        kInvalidFontFaceId,
        ScriptId::Zzzz,
        ShapingDirection::LeftToRight,
        FontFallbackSource::Missing,
        0U,
        0U});
    MultiRunCatalogHarfBuzzShapingRequest alternating_request =
        fixture.request(&face_cache);
    alternating_request.plan = &alternating_plan;
    MultiRunCatalogHarfBuzzShapingStats alternating_stats;
    MultiRunCatalogHarfBuzzShapingError alternating_error;
    ok &= expect(
        shape_multi_run_catalog_harfbuzz(
            alternating_request,
            &output,
            &alternating_stats,
            &alternating_error) &&
            output.segments.size() == cluster_count &&
            alternating_stats.completed_runs == cluster_count &&
            alternating_stats.distinct_bound_faces == 2U,
        "repeated non-adjacent face ids retain an exact distinct-face count");
    return ok && metadata_ledger.accounting_clean() &&
           glyph_ledger.accounting_clean();
}

bool test_concurrent_single_flight(const Fixture& fixture, std::size_t font_size) {
    PreparedHarfBuzzFaceCache face_cache(
        font_size * 6U,
        256U * 1024U,
        4U);
    constexpr std::size_t thread_count = 8U;
    std::array<bool, thread_count> results{};
    std::array<std::uint64_t, thread_count> glyph_counts{};
    std::atomic<std::size_t> ready{0U};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0U; index < thread_count; ++index) {
        threads.emplace_back([&, index] {
            ready.fetch_add(1U);
            while (!go.load()) {
                std::this_thread::yield();
            }
            ResourceLedger metadata_ledger;
            ResourceLedger glyph_ledger;
            metadata_ledger.set_hard_limit(
                ResourceClass::MultiRunShapeMetadata,
                256U * 1024U);
            glyph_ledger.set_hard_limit(
                ResourceClass::GlyphRun,
                2U * 1024U * 1024U);
            LedgerMemoryResource metadata_memory(
                metadata_ledger,
                ResourceClass::MultiRunShapeMetadata);
            LedgerMemoryResource glyph_memory(
                glyph_ledger,
                ResourceClass::GlyphRun);
            MultiRunShapedText output(&metadata_memory, &glyph_memory);
            MultiRunCatalogHarfBuzzShapingStats stats;
            MultiRunCatalogHarfBuzzShapingError error;
            results[index] = shape_multi_run_catalog_harfbuzz(
                                 fixture.request(&face_cache),
                                 &output,
                                 &stats,
                                 &error) &&
                             metadata_ledger.accounting_clean() &&
                             glyph_ledger.accounting_clean();
            glyph_counts[index] = stats.output_glyphs;
        });
    }
    while (ready.load() != thread_count) {
        std::this_thread::yield();
    }
    go.store(true);
    for (std::thread& thread : threads) {
        thread.join();
    }
    bool ok = true;
    for (std::size_t index = 0U; index < thread_count; ++index) {
        ok &= expect(
            results[index] && glyph_counts[index] == glyph_counts[0],
            "concurrent multi-run outputs agree");
    }
    const PreparedHarfBuzzFaceCacheStats cache_stats = face_cache.snapshot();
    return ok && expect(
        cache_stats.preparation_attempts == 2U &&
            cache_stats.faces_published == 2U &&
            cache_stats.entry_count == 2U,
        "concurrent multi-run calls single-flight both faces");
}

bool test_failures_and_rollback(
    const Fixture& fixture,
    const Fixture& other_generation,
    std::size_t font_size) {
    PreparedHarfBuzzFaceCache face_cache(
        font_size * 6U,
        256U * 1024U,
        4U);
    ResourceLedger metadata_ledger;
    ResourceLedger glyph_ledger;
    metadata_ledger.set_hard_limit(
        ResourceClass::MultiRunShapeMetadata,
        256U * 1024U);
    glyph_ledger.set_hard_limit(ResourceClass::GlyphRun, 2U * 1024U * 1024U);
    LedgerMemoryResource metadata_memory(
        metadata_ledger,
        ResourceClass::MultiRunShapeMetadata);
    LedgerMemoryResource glyph_memory(glyph_ledger, ResourceClass::GlyphRun);
    MultiRunShapedText output(&metadata_memory, &glyph_memory);
    MultiRunCatalogHarfBuzzShapingStats stats;
    MultiRunCatalogHarfBuzzShapingError error;
    if (!shape_multi_run_catalog_harfbuzz(
            fixture.request(&face_cache),
            &output,
            &stats,
            &error)) {
        return false;
    }

    ShapingRunPlan missing_plan;
    missing_plan.boundaries = fixture.plan.boundaries;
    missing_plan.boundaries[1U].face_id = kInvalidFontFaceId;
    missing_plan.boundaries[1U].fallback_source = FontFallbackSource::Missing;
    auto request = fixture.request(&face_cache);
    request.plan = &missing_plan;
    bool ok = expect(
        !shape_multi_run_catalog_harfbuzz(request, &output, &stats, &error) &&
            error.kind ==
                MultiRunCatalogHarfBuzzShapingErrorKind::MissingFontRun &&
            output.segments.empty(),
        "missing-font run fails closed and clears stale output");

    request = fixture.request(&face_cache);
    request.bindings = std::span(fixture.bindings).first(1U);
    ok &= expect(
        !shape_multi_run_catalog_harfbuzz(request, &output, &stats, &error) &&
            error.kind ==
                MultiRunCatalogHarfBuzzShapingErrorKind::FaceBindingNotFound &&
            error.face_id == 1U && output.segments.empty(),
        "missing face binding is exact and atomic");

    const std::array<CatalogFontFaceBinding, 2U> mixed_bindings{
        fixture.bindings[0],
        other_generation.bindings[1]};
    request = fixture.request(&face_cache);
    request.bindings = mixed_bindings;
    ok &= expect(
        !shape_multi_run_catalog_harfbuzz(request, &output, &stats, &error) &&
            error.kind ==
                MultiRunCatalogHarfBuzzShapingErrorKind::GenerationMismatch &&
            output.segments.empty(),
        "mixed catalog generations are rejected before shaping");

    ResourceLedger tiny_metadata_ledger;
    ResourceLedger normal_glyph_ledger;
    tiny_metadata_ledger.set_hard_limit(
        ResourceClass::MultiRunShapeMetadata,
        1U);
    normal_glyph_ledger.set_hard_limit(
        ResourceClass::GlyphRun,
        2U * 1024U * 1024U);
    LedgerMemoryResource tiny_metadata_memory(
        tiny_metadata_ledger,
        ResourceClass::MultiRunShapeMetadata);
    LedgerMemoryResource normal_glyph_memory(
        normal_glyph_ledger,
        ResourceClass::GlyphRun);
    MultiRunShapedText tiny_metadata_output(
        &tiny_metadata_memory,
        &normal_glyph_memory);
    request = fixture.request(&face_cache);
    ok &= expect(
        !shape_multi_run_catalog_harfbuzz(
            request,
            &tiny_metadata_output,
            &stats,
            &error) &&
            error.kind == MultiRunCatalogHarfBuzzShapingErrorKind::
                              MetadataBudgetExceeded &&
            tiny_metadata_output.segments.empty() &&
            tiny_metadata_ledger.snapshot(
                ResourceClass::MultiRunShapeMetadata)
                    .rejected_reservations == 1U,
        "one-byte metadata budget rejects before shaping");

    ResourceLedger first_run_ledger;
    first_run_ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource first_run_memory(
        first_run_ledger,
        ResourceClass::GlyphRun);
    ShapedGlyphRun first_run_output(&first_run_memory);
    CachedCatalogHarfBuzzShapingRequest first_run_request;
    first_run_request.binding = &fixture.bindings[0];
    first_run_request.prepared_face_cache = &face_cache;
    first_run_request.codepoints = fixture.codepoints;
    first_run_request.grapheme_boundaries = fixture.graphemes;
    first_run_request.cluster_limit = fixture.split_cluster;
    first_run_request.script = script("Latn");
    first_run_request.language = "und";
    first_run_request.beginning_of_text = true;
    CachedCatalogHarfBuzzShapingStats first_run_stats;
    CachedCatalogHarfBuzzShapingError first_run_error;
    if (!shape_cached_catalog_harfbuzz_segment(
            first_run_request,
            &first_run_output,
            &first_run_stats,
            &first_run_error)) {
        return false;
    }
    const std::size_t first_run_bytes =
        first_run_output.glyphs.size() * sizeof(ShapedGlyph);

    ResourceLedger rollback_metadata_ledger;
    ResourceLedger rollback_glyph_ledger;
    rollback_metadata_ledger.set_hard_limit(
        ResourceClass::MultiRunShapeMetadata,
        256U * 1024U);
    rollback_glyph_ledger.set_hard_limit(
        ResourceClass::GlyphRun,
        first_run_bytes);
    LedgerMemoryResource rollback_metadata_memory(
        rollback_metadata_ledger,
        ResourceClass::MultiRunShapeMetadata);
    LedgerMemoryResource rollback_glyph_memory(
        rollback_glyph_ledger,
        ResourceClass::GlyphRun);
    MultiRunShapedText rollback_output(
        &rollback_metadata_memory,
        &rollback_glyph_memory);
    ok &= expect(
        !shape_multi_run_catalog_harfbuzz(
            fixture.request(&face_cache),
            &rollback_output,
            &stats,
            &error) &&
            error.kind == MultiRunCatalogHarfBuzzShapingErrorKind::
                              SegmentShapingFailed &&
            error.run_index == 1U &&
            error.segment_error.shaping_error.shaping_error.kind ==
                HarfBuzzShapingErrorKind::OutputBudgetExceeded &&
            rollback_output.segments.empty() &&
            rollback_metadata_ledger.snapshot(
                ResourceClass::MultiRunShapeMetadata)
                    .current_bytes == 0U &&
            rollback_glyph_ledger.snapshot(ResourceClass::GlyphRun)
                    .current_bytes == 0U,
        "second-run glyph-budget failure rolls back all prior output");

    return ok && metadata_ledger.accounting_clean() &&
           glyph_ledger.accounting_clean() &&
           tiny_metadata_ledger.accounting_clean() &&
           normal_glyph_ledger.accounting_clean() &&
           rollback_metadata_ledger.accounting_clean() &&
           rollback_glyph_ledger.accounting_clean();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const std::filesystem::path font_path(argv[1]);
    std::error_code error_code;
    const std::uintmax_t raw_size =
        std::filesystem::file_size(font_path, error_code);
    if (error_code || raw_size == 0U ||
        raw_size > std::numeric_limits<std::size_t>::max() / 12U) {
        return 2;
    }
    const std::size_t font_size = static_cast<std::size_t>(raw_size);
    VerifiedFontResourceCache resource_cache(
        font_size * 8U,
        256U * 1024U,
        8U);
    Fixture fixture;
    Fixture other_generation;
    if (!fixture.build(font_path, font_size, 2201U, &resource_cache) ||
        !other_generation.build(font_path, font_size, 2202U, &resource_cache)) {
        return 2;
    }

    const bool ok =
        test_success_repeat_and_direct_equivalence(fixture, font_size) &&
        test_concurrent_single_flight(fixture, font_size) &&
        test_failures_and_rollback(fixture, other_generation, font_size);
    if (!ok) {
        return 1;
    }
    std::cout << "bounded multi-run HarfBuzz shaping tests passed\n";
    return 0;
}
