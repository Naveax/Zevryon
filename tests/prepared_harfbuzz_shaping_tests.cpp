#include "prepared_harfbuzz_face.hpp"

#include "grapheme_segmenter.hpp"
#include "harfbuzz_shaper.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_script.hpp"
#include "unicode_stream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

constexpr std::size_t kDiscoveryLimit = 2U * 1024U * 1024U;
constexpr std::size_t kCatalogLimit = 256U * 1024U;
constexpr std::size_t kGlyphLimit = 1024U * 1024U;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

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
    (void)script_id_from_name("Latn", &script);
    return script;
}

struct Fixture {
    std::pmr::monotonic_buffer_resource text_memory;
    std::pmr::vector<DecodedCodePoint> codepoints{&text_memory};
    std::pmr::vector<GraphemeBoundary> graphemes{&text_memory};
    std::string identity;
    std::string family{"Prepared Shape Family"};
    std::array<FontCoverageRange, 1> coverage{{{0x0020U, 0x007eU}}};
    std::array<FontDiscoveryFace, 1> faces;
    std::shared_ptr<const FontCatalogGeneration> generation;
    CatalogFontFaceBinding binding;
    std::shared_ptr<const PreparedHarfBuzzFace> prepared;

    bool build(
        const std::filesystem::path& font_path,
        std::size_t font_size,
        VerifiedFontResourceCache* cache) {
        constexpr std::string_view kText = "office affine a\xCC\x81";
        Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
        Utf8DecodeError decode_error;
        const auto bytes = std::as_bytes(std::span(kText.data(), kText.size()));
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

        identity = "fontconfig|";
        append_field(&identity, "");
        append_field(&identity, path_utf8(font_path));
        append_field(&identity, "0");
        append_field(&identity, "PreparedShapePS");
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
        FontDiscoveryStats discovery_stats;
        FontDiscoveryError discovery_error;
        if (!build_font_catalog_generation(
                1010U,
                faces,
                kDiscoveryLimit,
                kCatalogLimit,
                &generation,
                &discovery_stats,
                &discovery_error)) {
            return false;
        }

        CatalogFontResourceStats binding_stats;
        CatalogFontResourceError binding_error;
        if (!bind_catalog_font_face(
                generation,
                0U,
                font_size * 2U,
                cache,
                &binding,
                &binding_stats,
                &binding_error)) {
            return false;
        }
        PreparedHarfBuzzFaceStats prepared_stats;
        PreparedHarfBuzzFaceError prepared_error;
        return prepare_harfbuzz_face(
            binding,
            &prepared,
            &prepared_stats,
            &prepared_error);
    }

    BoundCatalogHarfBuzzShapingRequest bound_request(
        bool prepared_mode) const {
        BoundCatalogHarfBuzzShapingRequest value;
        value.binding = prepared_mode ? nullptr : &binding;
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
        value.prepared_harfbuzz_face = prepared_mode ? prepared : nullptr;
        return value;
    }

    HarfBuzzShapingRequest backend_request() const {
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

std::vector<ShapedGlyph> glyphs(const ShapedGlyphRun& run) {
    return std::vector<ShapedGlyph>(run.glyphs.begin(), run.glyphs.end());
}

bool catalog_paths_are_equivalent(Fixture* fixture) {
    ResourceLedger prepared_ledger;
    prepared_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource prepared_memory(
        prepared_ledger,
        ResourceClass::GlyphRun);
    ShapedGlyphRun prepared_output(&prepared_memory);
    BoundCatalogHarfBuzzShapingStats prepared_stats;
    BoundCatalogHarfBuzzShapingError prepared_error;

    bool ok = expect(
        shape_bound_catalog_harfbuzz_segment(
            fixture->bound_request(true),
            &prepared_output,
            &prepared_stats,
            &prepared_error),
        "prepared catalog face must shape");
    ok &= expect(
        prepared_stats.generation_id == fixture->prepared->generation_id() &&
            prepared_stats.face_id == fixture->prepared->face_id() &&
            prepared_stats.resource_id == fixture->prepared->resource_id() &&
            prepared_stats.shaping.used_prepared_harfbuzz_face &&
            prepared_stats.shaping.used_verified_font_resource &&
            !prepared_stats.shaping.performed_inline_font_verification &&
            prepared_stats.shaping.glyph_count_before_shaping ==
                fixture->prepared->glyph_count() &&
            prepared_stats.shaping.units_per_em ==
                fixture->prepared->units_per_em() &&
            prepared_stats.shaping_completed &&
            !prepared_output.glyphs.empty(),
        "prepared catalog path must publish exact native-face evidence");

    ResourceLedger binding_ledger;
    binding_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource binding_memory(
        binding_ledger,
        ResourceClass::GlyphRun);
    ShapedGlyphRun binding_output(&binding_memory);
    BoundCatalogHarfBuzzShapingStats binding_stats;
    BoundCatalogHarfBuzzShapingError binding_error;
    ok &= expect(
        shape_bound_catalog_harfbuzz_segment(
            fixture->bound_request(false),
            &binding_output,
            &binding_stats,
            &binding_error),
        "plain catalog binding must shape");
    ok &= expect(
        !binding_stats.shaping.used_prepared_harfbuzz_face &&
            binding_stats.shaping.used_verified_font_resource &&
            glyphs(prepared_output) == glyphs(binding_output) &&
            prepared_output.x_scale == binding_output.x_scale &&
            prepared_output.y_scale == binding_output.y_scale &&
            prepared_stats.shaping.total_x_advance ==
                binding_stats.shaping.total_x_advance &&
            prepared_stats.shaping.total_y_advance ==
                binding_stats.shaping.total_y_advance,
        "prepared and plain catalog paths must be byte-exact equivalents");
    return ok && prepared_ledger.accounting_clean() &&
        binding_ledger.accounting_clean();
}

bool isolated_font_state(Fixture* fixture) {
    auto shape_at_scale = [&](std::int32_t scale,
                              ShapedGlyphRun* output,
                              BoundCatalogHarfBuzzShapingStats* stats,
                              BoundCatalogHarfBuzzShapingError* error) {
        BoundCatalogHarfBuzzShapingRequest request =
            fixture->bound_request(true);
        request.x_scale = scale;
        request.y_scale = scale;
        return shape_bound_catalog_harfbuzz_segment(
            request,
            output,
            stats,
            error);
    };

    ResourceLedger first_ledger;
    first_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource first_memory(first_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun first_output(&first_memory);
    BoundCatalogHarfBuzzShapingStats first_stats;
    BoundCatalogHarfBuzzShapingError first_error;

    ResourceLedger second_ledger;
    second_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource second_memory(second_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun second_output(&second_memory);
    BoundCatalogHarfBuzzShapingStats second_stats;
    BoundCatalogHarfBuzzShapingError second_error;

    bool ok = expect(
        shape_at_scale(1000, &first_output, &first_stats, &first_error) &&
            shape_at_scale(2000, &second_output, &second_stats, &second_error),
        "different per-call scales must shape");
    ok &= expect(
        first_output.x_scale == 1000 && second_output.x_scale == 2000 &&
            first_output.glyphs.size() == second_output.glyphs.size() &&
            first_stats.shaping.total_x_advance !=
                second_stats.shaping.total_x_advance,
        "per-call hb_font scale state must remain isolated");

    ResourceLedger repeat_ledger;
    repeat_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource repeat_memory(repeat_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun repeat_output(&repeat_memory);
    BoundCatalogHarfBuzzShapingStats repeat_stats;
    BoundCatalogHarfBuzzShapingError repeat_error;
    ok &= expect(
        shape_at_scale(1000, &repeat_output, &repeat_stats, &repeat_error) &&
            glyphs(first_output) == glyphs(repeat_output),
        "repeating an earlier scale must be byte-exact");
    return ok && first_ledger.accounting_clean() &&
        second_ledger.accounting_clean() && repeat_ledger.accounting_clean();
}

bool catalog_failures_are_atomic(Fixture* fixture) {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource memory(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun output(&memory);
    BoundCatalogHarfBuzzShapingStats stats;
    BoundCatalogHarfBuzzShapingError error;
    BoundCatalogHarfBuzzShapingRequest request =
        fixture->bound_request(true);
    bool ok = expect(
        shape_bound_catalog_harfbuzz_segment(
            request,
            &output,
            &stats,
            &error),
        "failure fixture must seed output");

    request.binding = &fixture->binding;
    ok &= expect(
        !shape_bound_catalog_harfbuzz_segment(
            request,
            &output,
            &stats,
            &error) &&
            output.glyphs.empty() &&
            error.kind ==
                BoundCatalogHarfBuzzShapingErrorKind::InvalidArgument,
        "binding and prepared source together must fail atomically");

    request = fixture->bound_request(true);
    request.cluster_limit =
        static_cast<std::uint32_t>(fixture->graphemes.size() + 1U);
    ok &= expect(
        !shape_bound_catalog_harfbuzz_segment(
            request,
            &output,
            &stats,
            &error) &&
            output.glyphs.empty() &&
            error.kind == BoundCatalogHarfBuzzShapingErrorKind::ShapingFailed &&
            error.shaping_error.kind == HarfBuzzShapingErrorKind::InvalidInput,
        "invalid prepared catalog cluster range must preserve nested failure");

    ResourceLedger tiny_ledger;
    tiny_ledger.set_hard_limit(ResourceClass::GlyphRun, 1U);
    LedgerMemoryResource tiny_memory(tiny_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun tiny_output(&tiny_memory);
    BoundCatalogHarfBuzzShapingStats tiny_stats;
    BoundCatalogHarfBuzzShapingError tiny_error;
    ok &= expect(
        !shape_bound_catalog_harfbuzz_segment(
            fixture->bound_request(true),
            &tiny_output,
            &tiny_stats,
            &tiny_error) &&
            tiny_output.glyphs.empty() &&
            tiny_error.kind ==
                BoundCatalogHarfBuzzShapingErrorKind::ShapingFailed &&
            tiny_error.shaping_error.kind ==
                HarfBuzzShapingErrorKind::OutputBudgetExceeded,
        "one-byte prepared catalog glyph budget must fail atomically");
    return ok && ledger.accounting_clean() && tiny_ledger.accounting_clean();
}

bool backend_mode_failures_are_atomic(Fixture* fixture) {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource memory(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun output(&memory);
    HarfBuzzShapingStats stats;
    HarfBuzzShapingError error;
    HarfBuzzShapingRequest request = fixture->backend_request();
    request.prepared_harfbuzz_face = fixture->prepared;
    bool ok = expect(
        shape_harfbuzz_segment(request, &output, &stats, &error),
        "backend failure fixture must seed output");

    request.verified_font_resource = fixture->binding.resource();
    ok &= expect(
        !shape_harfbuzz_segment(request, &output, &stats, &error) &&
            output.glyphs.empty() &&
            error.kind == HarfBuzzShapingErrorKind::InvalidInput,
        "two backend font input modes must fail atomically");

    request.verified_font_resource.reset();
    request.face_index = 1U;
    ok &= expect(
        !shape_harfbuzz_segment(request, &output, &stats, &error) &&
            output.glyphs.empty() &&
            error.kind == HarfBuzzShapingErrorKind::InvalidInput,
        "prepared backend face-index mismatch must fail atomically");
    return ok && ledger.accounting_clean();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: prepared_harfbuzz_shaping_tests FONT\n";
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

    VerifiedFontResourceCache cache(
        static_cast<std::size_t>(file_size) * 3U,
        64U * 1024U,
        4U);
    Fixture fixture;
    if (!fixture.build(
            font_path,
            static_cast<std::size_t>(file_size),
            &cache)) {
        return 2;
    }
    fixture.generation.reset();
    cache.clear();

    bool ok = true;
    ok &= catalog_paths_are_equivalent(&fixture);
    ok &= isolated_font_state(&fixture);
    ok &= catalog_failures_are_atomic(&fixture);
    ok &= backend_mode_failures_are_atomic(&fixture);
    if (!ok) {
        return 1;
    }
    std::cout << "prepared catalog HarfBuzz shaping tests passed\n";
    return 0;
}
