#include "catalog_harfbuzz_shaper.hpp"
#include "grapheme_segmenter.hpp"
#include "harfbuzz_shaper.hpp"
#include "ledger_memory_resource.hpp"
#include "prepared_harfbuzz_face.hpp"
#include "resource_ledger.hpp"
#include "unicode_script.hpp"
#include "unicode_stream.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace zevryon::core;
using namespace zevryon::text;

constexpr std::size_t kFixtureBytes = 96U;
constexpr std::size_t kWarmupCalls = 128U;
constexpr std::size_t kBatchCalls = 128U;
constexpr std::size_t kSamples = 48U;
constexpr std::size_t kGlyphBudget = 1024U * 1024U;
constexpr std::size_t kDiscoveryLimit = 2U * 1024U * 1024U;
constexpr std::size_t kCatalogLimit = 256U * 1024U;

void append_field(std::string* identity, std::string_view value) {
    identity->append(std::to_string(value.size()));
    identity->push_back(':');
    identity->append(value);
    identity->push_back('|');
}

std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

ScriptId latin_script() {
    ScriptId script = ScriptId::Zzzz;
    return script_id_from_name("Latn", &script) ? script : ScriptId::Zzzz;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    const double position =
        fraction * static_cast<double>(sorted.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1U, sorted.size() - 1U);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

struct Fixture {
    std::string utf8;
    std::pmr::monotonic_buffer_resource text_memory;
    std::pmr::vector<DecodedCodePoint> codepoints{&text_memory};
    std::pmr::vector<GraphemeBoundary> graphemes{&text_memory};
    std::string identity;
    std::string family{"Prepared Benchmark Family"};
    std::array<FontCoverageRange, 1> coverage{{{0x0020U, 0x007eU}}};
    std::array<FontDiscoveryFace, 1> faces;
    CatalogFontFaceBinding binding;
    std::shared_ptr<const PreparedHarfBuzzFace> prepared;

    bool build_text() {
        constexpr std::string_view pattern = "office affine a\xCC\x81 ";
        while (utf8.size() + pattern.size() <= kFixtureBytes) {
            utf8.append(pattern);
        }
        Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
        Utf8DecodeError decode_error;
        const auto bytes = std::as_bytes(std::span(utf8.data(), utf8.size()));
        if (utf8.empty() ||
            !decoder.feed(bytes, 0U, &codepoints, &decode_error) ||
            !decoder.finish(&codepoints, &decode_error)) {
            return false;
        }
        GraphemeSegmentStats stats;
        GraphemeError error;
        return segment_graphemes(codepoints, &graphemes, &stats, &error) &&
            graphemes.size() >= 2U;
    }

    bool build_font(
        const std::filesystem::path& path,
        std::size_t font_size) {
        identity = "fontconfig|";
        append_field(&identity, "");
        append_field(&identity, path_utf8(path));
        append_field(&identity, "0");
        append_field(&identity, "PreparedBenchmarkPS");
        append_field(&identity, "");
        faces[0] = FontDiscoveryFace{
            identity,
            family,
            400U,
            5U,
            FontSlant::Upright,
            latin_script(),
            0U,
            coverage};

        std::shared_ptr<const FontCatalogGeneration> generation;
        FontDiscoveryStats discovery_stats;
        FontDiscoveryError discovery_error;
        if (!build_font_catalog_generation(
                1110U,
                faces,
                kDiscoveryLimit,
                kCatalogLimit,
                &generation,
                &discovery_stats,
                &discovery_error)) {
            return false;
        }

        VerifiedFontResourceCache cache(
            font_size * 3U,
            64U * 1024U,
            4U);
        CatalogFontResourceStats binding_stats;
        CatalogFontResourceError binding_error;
        if (!bind_catalog_font_face(
                generation,
                0U,
                font_size * 2U,
                &cache,
                &binding,
                &binding_stats,
                &binding_error)) {
            return false;
        }
        PreparedHarfBuzzFaceStats prepared_stats;
        PreparedHarfBuzzFaceError prepared_error;
        if (!prepare_harfbuzz_face(
                binding,
                &prepared,
                &prepared_stats,
                &prepared_error)) {
            return false;
        }
        generation.reset();
        cache.clear();
        return binding.valid() && prepared != nullptr && prepared->valid();
    }

    HarfBuzzShapingRequest request() const {
        HarfBuzzShapingRequest value;
        value.face_index = 0U;
        value.codepoints = codepoints;
        value.grapheme_boundaries = graphemes;
        value.first_cluster = 0U;
        value.cluster_limit =
            static_cast<std::uint32_t>(graphemes.size() - 1U);
        value.script = latin_script();
        value.direction = ShapingDirection::LeftToRight;
        value.language = "en";
        value.beginning_of_text = true;
        value.end_of_text = true;
        return value;
    }
};

struct Measurement {
    explicit Measurement()
        : memory(ledger, ResourceClass::GlyphRun), output(&memory) {
        ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphBudget);
    }

    ResourceLedger ledger;
    LedgerMemoryResource memory;
    ShapedGlyphRun output;
    HarfBuzzShapingStats stats;
    HarfBuzzShapingError error;
    std::vector<double> samples;
};

bool run_calls(
    const HarfBuzzShapingRequest& request,
    std::size_t count,
    Measurement* measurement) {
    for (std::size_t index = 0U; index < count; ++index) {
        if (!shape_harfbuzz_segment(
                request,
                &measurement->output,
                &measurement->stats,
                &measurement->error)) {
            std::cerr << measurement->error.message << '\n';
            return false;
        }
    }
    return true;
}

double measure_batch(
    const HarfBuzzShapingRequest& request,
    Measurement* measurement,
    bool* ok) {
    const auto begin = std::chrono::steady_clock::now();
    *ok = run_calls(request, kBatchCalls, measurement);
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - begin).count() /
        static_cast<double>(kBatchCalls);
}

std::vector<ShapedGlyph> copy_glyphs(const ShapedGlyphRun& output) {
    return std::vector<ShapedGlyph>(output.glyphs.begin(), output.glyphs.end());
}

bool validate_measurement(
    const Measurement& measurement,
    bool prepared_mode,
    std::uint64_t resource_id) {
    const ResourceSnapshot snapshot =
        measurement.ledger.snapshot(ResourceClass::GlyphRun);
    const std::size_t output_bytes =
        measurement.output.glyphs.size() * sizeof(ShapedGlyph);
    return !measurement.output.glyphs.empty() &&
        measurement.stats.missing_glyphs == 0U &&
        measurement.stats.used_verified_font_resource &&
        !measurement.stats.performed_inline_font_verification &&
        measurement.stats.used_prepared_harfbuzz_face == prepared_mode &&
        measurement.stats.verified_font_resource_id == resource_id &&
        snapshot.current_bytes == output_bytes &&
        snapshot.rejected_reservations == 0U &&
        snapshot.accounting_errors == 0U &&
        measurement.ledger.accounting_clean() &&
        measurement.ledger.within_hard_limits();
}

void emit(
    std::string_view mode,
    const Fixture& fixture,
    Measurement* measurement) {
    std::sort(measurement->samples.begin(), measurement->samples.end());
    const ResourceSnapshot snapshot =
        measurement->ledger.snapshot(ResourceClass::GlyphRun);
    const std::size_t output_bytes =
        measurement->output.glyphs.size() * sizeof(ShapedGlyph);
    std::cout << std::fixed << std::setprecision(9)
              << "{\"schema\":\"zevryon.prepared-harfbuzz-shaping-benchmark.v1\","
              << "\"mode\":\"" << mode << "\","
              << "\"input_utf8_bytes\":" << fixture.utf8.size() << ','
              << "\"input_codepoints\":" << fixture.codepoints.size() << ','
              << "\"input_clusters\":" << fixture.graphemes.size() - 1U << ','
              << "\"output_glyphs\":" << measurement->output.glyphs.size() << ','
              << "\"output_bytes\":" << output_bytes << ','
              << "\"total_x_advance\":"
              << measurement->stats.total_x_advance << ','
              << "\"total_y_advance\":"
              << measurement->stats.total_y_advance << ','
              << "\"units_per_em\":" << measurement->stats.units_per_em << ','
              << "\"verified_font_resource_id\":"
              << measurement->stats.verified_font_resource_id << ','
              << "\"used_prepared_harfbuzz_face\":"
              << (measurement->stats.used_prepared_harfbuzz_face
                      ? "true" : "false") << ','
              << "\"samples\":" << kSamples << ','
              << "\"calls_per_sample\":" << kBatchCalls << ','
              << "\"p50_ms\":"
              << percentile(measurement->samples, 0.50) << ','
              << "\"p95_ms\":"
              << percentile(measurement->samples, 0.95) << ','
              << "\"p99_ms\":"
              << percentile(measurement->samples, 0.99) << ','
              << "\"maximum_ms\":" << measurement->samples.back() << ','
              << "\"glyph_current_bytes\":" << snapshot.current_bytes << ','
              << "\"glyph_peak_bytes\":" << snapshot.peak_bytes << ','
              << "\"accounting_clean\":true,"
              << "\"within_hard_limits\":true}"
              << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: prepared_harfbuzz_shaping_benchmark FONT\n";
        return 2;
    }
    const std::filesystem::path font_path(argv[1]);
    std::error_code filesystem_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(font_path, filesystem_error);
    if (filesystem_error || file_size == 0U ||
        file_size > static_cast<std::uintmax_t>(
                        std::numeric_limits<std::size_t>::max() / 4U)) {
        return 2;
    }

    Fixture fixture;
    if (!fixture.build_text() ||
        !fixture.build_font(
            font_path,
            static_cast<std::size_t>(file_size))) {
        return 2;
    }

    HarfBuzzShapingRequest verified_request = fixture.request();
    verified_request.verified_font_resource = fixture.binding.resource();
    HarfBuzzShapingRequest prepared_request = fixture.request();
    prepared_request.prepared_harfbuzz_face = fixture.prepared;

    Measurement verified;
    Measurement prepared;
    if (!run_calls(verified_request, kWarmupCalls, &verified) ||
        !run_calls(prepared_request, kWarmupCalls, &prepared)) {
        return 1;
    }
    verified.samples.reserve(kSamples);
    prepared.samples.reserve(kSamples);
    for (std::size_t sample = 0U; sample < kSamples; ++sample) {
        bool first_ok = false;
        bool second_ok = false;
        if ((sample & 1U) == 0U) {
            verified.samples.push_back(
                measure_batch(verified_request, &verified, &first_ok));
            prepared.samples.push_back(
                measure_batch(prepared_request, &prepared, &second_ok));
        } else {
            prepared.samples.push_back(
                measure_batch(prepared_request, &prepared, &first_ok));
            verified.samples.push_back(
                measure_batch(verified_request, &verified, &second_ok));
        }
        if (!first_ok || !second_ok) {
            return 1;
        }
    }

    if (!validate_measurement(
            verified,
            false,
            fixture.binding.resource_id()) ||
        !validate_measurement(
            prepared,
            true,
            fixture.binding.resource_id()) ||
        copy_glyphs(verified.output) != copy_glyphs(prepared.output) ||
        verified.stats.total_x_advance != prepared.stats.total_x_advance ||
        verified.stats.total_y_advance != prepared.stats.total_y_advance ||
        verified.stats.units_per_em != prepared.stats.units_per_em) {
        std::cerr << "prepared and verified benchmark paths diverged\n";
        return 1;
    }

    emit("verified_resource_call", fixture, &verified);
    emit("prepared_face_call", fixture, &prepared);
    return 0;
}
