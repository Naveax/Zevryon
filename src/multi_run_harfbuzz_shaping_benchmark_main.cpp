#include "ledger_memory_resource.hpp"
#include "multi_run_harfbuzz_shaper.hpp"
#include "resource_ledger.hpp"
#include "unicode_script.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
using namespace zevryon::core;
using namespace zevryon::text;

constexpr std::size_t kPairs = 512U;
constexpr std::size_t kLatinClusters = 64U;
constexpr std::size_t kArabicClusters = 32U;
constexpr std::size_t kWarmups = 8U;
constexpr std::size_t kBatch = 4U;
constexpr std::size_t kSamples = 32U;
constexpr std::size_t kMetadataLimit = 512U * 1024U;
constexpr std::size_t kGlyphLimit = 4U * 1024U * 1024U;

void append_field(std::string* output, std::string_view value) {
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

double percentile(const std::vector<double>& values, double probability) {
    const double position = probability *
        static_cast<double>(values.size() - 1U);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1U, values.size() - 1U);
    const double weight = position - static_cast<double>(lower);
    return values[lower] * (1.0 - weight) + values[upper] * weight;
}

struct Fixture {
    std::pmr::monotonic_buffer_resource text_memory;
    std::pmr::vector<DecodedCodePoint> codepoints{&text_memory};
    std::pmr::vector<GraphemeBoundary> graphemes{&text_memory};
    ShapingRunPlan plan;
    std::array<std::string, 2U> identities;
    std::array<std::string, 2U> families{{"Bench Latin", "Bench Arabic"}};
    std::array<FontCoverageRange, 2U> coverage{{
        {0x20U, 0x7eU},
        {0x0600U, 0x06ffU},
    }};
    std::array<FontDiscoveryFace, 2U> faces;
    std::array<CatalogFontFaceBinding, 2U> bindings;
    std::shared_ptr<const FontCatalogGeneration> generation;
    std::size_t utf8_bytes{0U};

    bool append_cluster(std::uint32_t codepoint, std::uint8_t bytes) {
        if (utf8_bytes > std::numeric_limits<std::uint64_t>::max() - bytes ||
            codepoints.size() >=
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
            return false;
        }
        graphemes.push_back(GraphemeBoundary{
            utf8_bytes,
            static_cast<std::uint32_t>(codepoints.size())});
        codepoints.emplace_back(
            codepoint,
            utf8_bytes,
            utf8_bytes + bytes,
            false);
        utf8_bytes += bytes;
        return true;
    }

    bool build(
        const std::filesystem::path& font_path,
        std::size_t font_size,
        VerifiedFontResourceCache* resource_cache) {
        const ScriptId latin = script("Latn");
        const ScriptId arabic = script("Arab");
        for (std::size_t pair = 0U; pair < kPairs; ++pair) {
            const auto latin_start = static_cast<std::uint32_t>(
                graphemes.size());
            plan.boundaries.push_back(ShapingRunBoundary{
                latin_start,
                0U,
                latin,
                ShapingDirection::LeftToRight,
                FontFallbackSource::Primary,
                0U,
                0U});
            for (std::size_t index = 0U; index < kLatinClusters; ++index) {
                const std::uint32_t value = static_cast<std::uint32_t>(
                    'a' + static_cast<int>(index % 20U));
                if (!append_cluster(value, 1U)) {
                    return false;
                }
            }

            const auto arabic_start = static_cast<std::uint32_t>(
                graphemes.size());
            plan.boundaries.push_back(ShapingRunBoundary{
                arabic_start,
                1U,
                arabic,
                ShapingDirection::RightToLeft,
                FontFallbackSource::ScriptMatch,
                1U,
                0U});
            for (std::size_t index = 0U; index < kArabicClusters; ++index) {
                const std::uint32_t value = 0x0627U +
                    static_cast<std::uint32_t>(index % 12U);
                if (!append_cluster(value, 2U)) {
                    return false;
                }
            }
        }
        graphemes.push_back(GraphemeBoundary{
            utf8_bytes,
            static_cast<std::uint32_t>(codepoints.size())});
        plan.boundaries.push_back(ShapingRunBoundary{
            static_cast<std::uint32_t>(graphemes.size() - 1U),
            kInvalidFontFaceId,
            ScriptId::Zzzz,
            ShapingDirection::LeftToRight,
            FontFallbackSource::Missing,
            0U,
            0U});
        if (utf8_bytes != 64U * 1024U ||
            codepoints.size() != 49'152U ||
            plan.boundaries.size() != 1'025U) {
            return false;
        }

        const std::string path = utf8_path(font_path);
        for (std::size_t index = 0U; index < identities.size(); ++index) {
            identities[index] = "fontconfig|";
            append_field(&identities[index], "");
            append_field(&identities[index], path);
            append_field(&identities[index], "0");
            append_field(
                &identities[index],
                index == 0U ? "ZevryonBenchA" : "ZevryonBenchB");
            append_field(&identities[index], "");
        }
        faces[0] = FontDiscoveryFace{
            identities[0], families[0], 400U, 5U, FontSlant::Upright,
            latin, 0U, coverage};
        faces[1] = FontDiscoveryFace{
            identities[1], families[1], 400U, 5U, FontSlant::Upright,
            arabic, 0U, coverage};

        FontDiscoveryStats discovery_stats;
        FontDiscoveryError discovery_error;
        if (!build_font_catalog_generation(
                2301U,
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
        generation.reset();
        resource_cache->clear();
        return true;
    }

    MultiRunCatalogHarfBuzzShapingRequest executor_request(
        PreparedHarfBuzzFaceCache* face_cache) const {
        MultiRunCatalogHarfBuzzShapingRequest request;
        request.plan = &plan;
        request.bindings = bindings;
        request.prepared_face_cache = face_cache;
        request.codepoints = codepoints;
        request.grapheme_boundaries = graphemes;
        request.language = "und";
        return request;
    }
};

struct Measure {
    Measure()
        : metadata_memory(
              metadata_ledger,
              ResourceClass::MultiRunShapeMetadata),
          glyph_memory(glyph_ledger, ResourceClass::GlyphRun),
          output(&metadata_memory, &glyph_memory) {
        metadata_ledger.set_hard_limit(
            ResourceClass::MultiRunShapeMetadata,
            kMetadataLimit);
        glyph_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    }

    ResourceLedger metadata_ledger;
    ResourceLedger glyph_ledger;
    LedgerMemoryResource metadata_memory;
    LedgerMemoryResource glyph_memory;
    MultiRunShapedText output;
    std::uint64_t glyphs{0U};
    std::int64_t x_advance{0};
    std::uint64_t checksum{0U};
    std::vector<double> samples;
};

bool aggregate_output(Measure* measure) {
    std::uint64_t glyphs = 0U;
    std::int64_t advance = 0;
    std::uint64_t checksum = 1469598103934665603ULL;
    for (const MultiRunShapedSegment& segment : measure->output.segments) {
        for (const ShapedGlyph& glyph : segment.glyphs.glyphs) {
            if (glyphs == std::numeric_limits<std::uint64_t>::max()) {
                return false;
            }
            ++glyphs;
            const std::int64_t widened = glyph.x_advance;
            if ((widened > 0 &&
                 advance > std::numeric_limits<std::int64_t>::max() - widened) ||
                (widened < 0 &&
                 advance < std::numeric_limits<std::int64_t>::min() - widened)) {
                return false;
            }
            advance += widened;
            checksum ^= glyph.glyph_id;
            checksum *= 1099511628211ULL;
            checksum ^= glyph.cluster_index;
            checksum *= 1099511628211ULL;
            checksum ^= static_cast<std::uint32_t>(glyph.x_advance);
            checksum *= 1099511628211ULL;
            checksum ^= glyph.flags;
            checksum *= 1099511628211ULL;
        }
    }
    measure->glyphs = glyphs;
    measure->x_advance = advance;
    measure->checksum = checksum;
    return true;
}

bool call_executor(
    const Fixture& fixture,
    PreparedHarfBuzzFaceCache* face_cache,
    Measure* measure) {
    MultiRunCatalogHarfBuzzShapingStats stats;
    MultiRunCatalogHarfBuzzShapingError error;
    if (!shape_multi_run_catalog_harfbuzz(
            fixture.executor_request(face_cache),
            &measure->output,
            &stats,
            &error) ||
        !aggregate_output(measure)) {
        return false;
    }
    return stats.input_runs == 1'024U &&
           stats.completed_runs == 1'024U &&
           stats.output_glyphs == measure->glyphs &&
           stats.total_x_advance == measure->x_advance;
}

bool call_manual(
    const Fixture& fixture,
    PreparedHarfBuzzFaceCache* face_cache,
    Measure* measure) {
    measure->output.release();
    const std::size_t run_count = fixture.plan.boundaries.size() - 1U;
    try {
        measure->output.segments.reserve(run_count);
    } catch (...) {
        return false;
    }
    for (std::size_t run_index = 0U; run_index < run_count; ++run_index) {
        const ShapingRunBoundary& current =
            fixture.plan.boundaries[run_index];
        const ShapingRunBoundary& next =
            fixture.plan.boundaries[run_index + 1U];
        try {
            measure->output.segments.emplace_back(
                measure->output.glyph_resource());
        } catch (...) {
            measure->output.release();
            return false;
        }
        MultiRunShapedSegment& segment = measure->output.segments.back();
        segment.run = current;
        CachedCatalogHarfBuzzShapingRequest request;
        request.binding = &fixture.bindings[current.face_id];
        request.prepared_face_cache = face_cache;
        request.codepoints = fixture.codepoints;
        request.grapheme_boundaries = fixture.graphemes;
        request.first_cluster = current.cluster_index;
        request.cluster_limit = next.cluster_index;
        request.script = current.script;
        request.direction = current.direction;
        request.language = "und";
        request.beginning_of_text = run_index == 0U;
        request.end_of_text = run_index + 1U == run_count;
        CachedCatalogHarfBuzzShapingStats stats;
        CachedCatalogHarfBuzzShapingError error;
        if (!shape_cached_catalog_harfbuzz_segment(
                request,
                &segment.glyphs,
                &stats,
                &error)) {
            measure->output.release();
            return false;
        }
    }
    return aggregate_output(measure);
}

template <typename Function>
bool repeat(Function&& function, std::size_t count) {
    for (std::size_t index = 0U; index < count; ++index) {
        if (!function()) {
            return false;
        }
    }
    return true;
}

template <typename Function>
double measure_batch(Function&& function, bool* success) {
    const auto start = std::chrono::steady_clock::now();
    *success = repeat(function, kBatch);
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count() /
           static_cast<double>(kBatch);
}

bool valid_measure(const Measure& measure, std::size_t run_count) {
    const ResourceSnapshot metadata = measure.metadata_ledger.snapshot(
        ResourceClass::MultiRunShapeMetadata);
    const ResourceSnapshot glyphs = measure.glyph_ledger.snapshot(
        ResourceClass::GlyphRun);
    const std::size_t expected_glyph_bytes =
        static_cast<std::size_t>(measure.glyphs) * sizeof(ShapedGlyph);
    return measure.output.segments.size() == run_count &&
           measure.glyphs != 0U &&
           glyphs.current_bytes == expected_glyph_bytes &&
           metadata.current_bytes != 0U &&
           metadata.current_bytes <= kMetadataLimit &&
           glyphs.current_bytes <= kGlyphLimit &&
           measure.metadata_ledger.accounting_clean() &&
           measure.glyph_ledger.accounting_clean() &&
           measure.metadata_ledger.within_hard_limits() &&
           measure.glyph_ledger.within_hard_limits();
}

void emit(std::string_view mode, const Fixture& fixture, Measure* measure) {
    std::sort(measure->samples.begin(), measure->samples.end());
    const ResourceSnapshot metadata = measure->metadata_ledger.snapshot(
        ResourceClass::MultiRunShapeMetadata);
    const ResourceSnapshot glyphs = measure->glyph_ledger.snapshot(
        ResourceClass::GlyphRun);
    std::cout << std::fixed << std::setprecision(9)
              << "{\"schema\":\"zevryon.multi-run-shaping-benchmark.v1\","
              << "\"mode\":\"" << mode << "\","
              << "\"input_utf8_bytes\":" << fixture.utf8_bytes << ','
              << "\"input_codepoints\":" << fixture.codepoints.size() << ','
              << "\"input_clusters\":" << fixture.graphemes.size() - 1U << ','
              << "\"input_runs\":" << fixture.plan.boundaries.size() - 1U << ','
              << "\"output_segments\":" << measure->output.segments.size() << ','
              << "\"output_glyphs\":" << measure->glyphs << ','
              << "\"glyph_bytes\":" << glyphs.current_bytes << ','
              << "\"metadata_bytes\":" << metadata.current_bytes << ','
              << "\"total_x_advance\":" << measure->x_advance << ','
              << "\"checksum\":" << measure->checksum << ','
              << "\"p50_ms\":" << percentile(measure->samples, 0.50) << ','
              << "\"p95_ms\":" << percentile(measure->samples, 0.95) << ','
              << "\"p99_ms\":" << percentile(measure->samples, 0.99) << ','
              << "\"maximum_ms\":" << measure->samples.back() << ','
              << "\"accounting_clean\":true,"
              << "\"within_hard_limits\":true}\n";
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
    if (!fixture.build(font_path, font_size, &resource_cache)) {
        return 2;
    }
    PreparedHarfBuzzFaceCache face_cache(
        font_size * 8U,
        256U * 1024U,
        8U);
    for (const CatalogFontFaceBinding& binding : fixture.bindings) {
        std::shared_ptr<const PreparedHarfBuzzFace> prepared;
        PreparedHarfBuzzFaceCacheStats stats;
        PreparedHarfBuzzFaceCacheError error;
        if (!face_cache.get_or_prepare(binding, &prepared, &stats, &error)) {
            return 2;
        }
    }

    Measure manual;
    Measure executor;
    auto manual_call = [&] {
        return call_manual(fixture, &face_cache, &manual);
    };
    auto executor_call = [&] {
        return call_executor(fixture, &face_cache, &executor);
    };
    if (!repeat(manual_call, kWarmups) ||
        !repeat(executor_call, kWarmups)) {
        return 1;
    }
    manual.samples.reserve(kSamples);
    executor.samples.reserve(kSamples);
    for (std::size_t sample = 0U; sample < kSamples; ++sample) {
        bool first = false;
        bool second = false;
        if ((sample & 1U) == 0U) {
            manual.samples.push_back(measure_batch(manual_call, &first));
            executor.samples.push_back(measure_batch(executor_call, &second));
        } else {
            executor.samples.push_back(measure_batch(executor_call, &first));
            manual.samples.push_back(measure_batch(manual_call, &second));
        }
        if (!first || !second) {
            return 1;
        }
    }

    const std::size_t run_count = fixture.plan.boundaries.size() - 1U;
    if (!valid_measure(manual, run_count) ||
        !valid_measure(executor, run_count) ||
        manual.glyphs != executor.glyphs ||
        manual.x_advance != executor.x_advance ||
        manual.checksum != executor.checksum) {
        return 1;
    }
    emit("manual_cached_segment_loop", fixture, &manual);
    emit("production_multi_run_executor", fixture, &executor);
    return 0;
}
