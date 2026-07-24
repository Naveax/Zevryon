#include "grapheme_segmenter.hpp"
#include "harfbuzz_shaper.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_script.hpp"
#include "unicode_stream.hpp"
#include "verified_font_resource.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace zevryon::core;
using namespace zevryon::text;

constexpr std::size_t kFixtureBytes = 16U * 1024U;
constexpr std::size_t kWarmupIterations = 4U;
constexpr std::size_t kMeasuredIterations = 64U;
constexpr std::size_t kGlyphBudget = 16U * 1024U * 1024U;

struct Fixture {
    std::string utf8;
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::vector<DecodedCodePoint> codepoints{&resource};
    std::pmr::vector<GraphemeBoundary> graphemes{&resource};
};

bool load_file(const char* path, std::vector<std::byte>* output) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return false;
    }
    const std::streampos end = stream.tellg();
    if (end <= 0 ||
        static_cast<std::uint64_t>(end) >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    output->resize(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    stream.read(
        reinterpret_cast<char*>(output->data()),
        static_cast<std::streamsize>(output->size()));
    return stream.good() || stream.eof();
}

bool build_fixture(std::string_view pattern, Fixture* fixture) {
    while (fixture->utf8.size() + pattern.size() <= kFixtureBytes) {
        fixture->utf8.append(pattern);
    }
    if (fixture->utf8.empty()) {
        return false;
    }

    Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
    Utf8DecodeError decode_error;
    const auto bytes = std::as_bytes(std::span(
        fixture->utf8.data(),
        fixture->utf8.size()));
    if (!decoder.feed(bytes, 0U, &fixture->codepoints, &decode_error) ||
        !decoder.finish(&fixture->codepoints, &decode_error)) {
        return false;
    }
    GraphemeSegmentStats grapheme_stats;
    GraphemeError grapheme_error;
    return segment_graphemes(
               fixture->codepoints,
               &fixture->graphemes,
               &grapheme_stats,
               &grapheme_error) &&
           fixture->graphemes.size() >= 2U;
}

ScriptId require_script(std::string_view name) {
    ScriptId script = ScriptId::Zzzz;
    return script_id_from_name(name, &script) ? script : ScriptId::Zzzz;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    const double position =
        fraction * static_cast<double>(sorted.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1U, sorted.size() - 1U);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

bool build_resource(
    std::uint64_t resource_id,
    std::span<const std::byte> font,
    std::shared_ptr<const VerifiedFontResource>* output) {
    VerifiedFontResourceError error;
    if (!build_verified_font_resource(
            resource_id,
            font,
            0U,
            font.size(),
            output,
            nullptr,
            &error)) {
        std::cerr << "verified resource build failed: " << error.message << '\n';
        return false;
    }
    return true;
}

bool run_case(
    std::string_view name,
    std::span<const std::byte> font,
    const std::shared_ptr<const VerifiedFontResource>& verified_resource,
    Fixture& fixture,
    ScriptId script,
    ShapingDirection direction,
    std::string_view language) {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphBudget);
    LedgerMemoryResource resource(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun output(&resource);
    HarfBuzzShapingStats stats;
    HarfBuzzShapingError error;
    std::vector<double> durations;
    durations.reserve(kMeasuredIterations);

    HarfBuzzShapingRequest request;
    request.font_bytes = verified_resource == nullptr
        ? font
        : std::span<const std::byte>{};
    request.face_index = 0U;
    request.codepoints = fixture.codepoints;
    request.grapheme_boundaries = fixture.graphemes;
    request.first_cluster = 0U;
    request.cluster_limit =
        static_cast<std::uint32_t>(fixture.graphemes.size() - 1U);
    request.script = script;
    request.direction = direction;
    request.language = language;
    request.beginning_of_text = true;
    request.end_of_text = true;
    request.produce_unsafe_to_concat = true;
    request.verified_font_resource = verified_resource;

    for (std::size_t iteration = 0U;
         iteration < kWarmupIterations + kMeasuredIterations;
         ++iteration) {
        const auto begin = std::chrono::steady_clock::now();
        const bool ok = shape_harfbuzz_segment(
            request,
            &output,
            &stats,
            &error);
        const auto end = std::chrono::steady_clock::now();
        if (!ok) {
            std::cerr << name << " shaping failed: " << error.message << '\n';
            return false;
        }
        if (iteration >= kWarmupIterations) {
            durations.push_back(
                std::chrono::duration<double, std::milli>(end - begin).count());
        }
    }

    std::sort(durations.begin(), durations.end());
    const ResourceSnapshot snapshot = ledger.snapshot(ResourceClass::GlyphRun);
    const std::size_t expected_output_bytes =
        output.glyphs.size() * sizeof(ShapedGlyph);
    const bool retained_mode = verified_resource != nullptr;
    if (stats.missing_glyphs != 0U ||
        snapshot.current_bytes != expected_output_bytes ||
        snapshot.rejected_reservations != 0U ||
        snapshot.accounting_errors != 0U ||
        !ledger.accounting_clean() ||
        !ledger.within_hard_limits() ||
        stats.used_verified_font_resource != retained_mode ||
        stats.performed_inline_font_verification == retained_mode ||
        stats.verified_font_resource_id !=
            (retained_mode ? verified_resource->resource_id() : 0U)) {
        std::cerr << name << " benchmark accounting or input-path failed\n";
        return false;
    }

    const double p50 = percentile(durations, 0.50);
    const double p95 = percentile(durations, 0.95);
    const double p99 = percentile(durations, 0.99);
    const double maximum = durations.back();
    const double throughput = p50 == 0.0
        ? 0.0
        : (static_cast<double>(fixture.utf8.size()) / (1024.0 * 1024.0)) /
              (p50 / 1000.0);

    std::cout << std::fixed << std::setprecision(6)
              << "{\"schema\":\"zevryon.harfbuzz-shaping-benchmark.v1\"," 
              << "\"mode\":\""
              << (retained_mode
                      ? "verified_resource_call"
                      : "uncached_full_call")
              << "\","
              << "\"case\":\"" << name << "\","
              << "\"font_bytes\":"
              << (retained_mode ? verified_resource->bytes().size() : font.size())
              << ','
              << "\"verified_font_resource_id\":"
              << stats.verified_font_resource_id << ','
              << "\"used_verified_font_resource\":"
              << (stats.used_verified_font_resource ? "true" : "false") << ','
              << "\"performed_inline_font_verification\":"
              << (stats.performed_inline_font_verification ? "true" : "false")
              << ','
              << "\"input_utf8_bytes\":" << fixture.utf8.size() << ','
              << "\"input_codepoints\":" << fixture.codepoints.size() << ','
              << "\"input_clusters\":" << fixture.graphemes.size() - 1U << ','
              << "\"output_glyphs\":" << output.glyphs.size() << ','
              << "\"output_bytes\":" << expected_output_bytes << ','
              << "\"unsafe_to_break_glyphs\":"
              << stats.unsafe_to_break_glyphs << ','
              << "\"unsafe_to_concat_glyphs\":"
              << stats.unsafe_to_concat_glyphs << ','
              << "\"total_x_advance\":" << stats.total_x_advance << ','
              << "\"total_y_advance\":" << stats.total_y_advance << ','
              << "\"units_per_em\":" << stats.units_per_em << ','
              << "\"iterations\":" << kMeasuredIterations << ','
              << "\"warmup_iterations\":" << kWarmupIterations << ','
              << "\"p50_ms\":" << p50 << ','
              << "\"p95_ms\":" << p95 << ','
              << "\"p99_ms\":" << p99 << ','
              << "\"maximum_ms\":" << maximum << ','
              << "\"p50_throughput_mib_s\":" << throughput << ','
              << "\"glyph_current_bytes\":" << snapshot.current_bytes << ','
              << "\"glyph_peak_bytes\":" << snapshot.peak_bytes << ','
              << "\"accounting_clean\":true,"
              << "\"within_hard_limits\":true}"
              << '\n';
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const bool verified_mode =
        argc == 4 && std::string_view(argv[3]) == "--verified-resource";
    if (argc != 3 && !verified_mode) {
        std::cerr
            << "usage: harfbuzz_shaping_benchmark LATIN_FONT DEVANAGARI_FONT "
               "[--verified-resource]\n";
        return 2;
    }

    std::vector<std::byte> latin_font;
    std::vector<std::byte> devanagari_font;
    if (!load_file(argv[1], &latin_font) ||
        !load_file(argv[2], &devanagari_font)) {
        return 2;
    }

    std::shared_ptr<const VerifiedFontResource> latin_resource;
    std::shared_ptr<const VerifiedFontResource> devanagari_resource;
    if (verified_mode &&
        (!build_resource(1001U, latin_font, &latin_resource) ||
         !build_resource(1002U, devanagari_font, &devanagari_resource))) {
        return 2;
    }

    Fixture latin;
    Fixture arabic;
    Fixture devanagari;
    if (!build_fixture("office a\xCC\x81 ", &latin) ||
        !build_fixture("\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 ", &arabic) ||
        !build_fixture(
            "\xE0\xA4\x95\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\xAE ",
            &devanagari)) {
        return 2;
    }

    bool ok = true;
    ok &= run_case(
        "latin",
        latin_font,
        latin_resource,
        latin,
        require_script("Latn"),
        ShapingDirection::LeftToRight,
        "en");
    ok &= run_case(
        "arabic",
        latin_font,
        latin_resource,
        arabic,
        require_script("Arab"),
        ShapingDirection::RightToLeft,
        "ar");
    ok &= run_case(
        "devanagari",
        devanagari_font,
        devanagari_resource,
        devanagari,
        require_script("Deva"),
        ShapingDirection::LeftToRight,
        "hi");
    return ok ? 0 : 1;
}
