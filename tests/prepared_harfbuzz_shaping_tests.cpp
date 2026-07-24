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

std::string fontconfig_identity(const std::filesystem::path& path) {
    std::string result = "fontconfig|";
    append_field(&result, "");
    append_field(&result, path_utf8(path));
    append_field(&result, "0");
    append_field(&result, "PreparedShapePS");
    append_field(&result, "");
    return result;
}

ScriptId latin_script() {
    ScriptId script = ScriptId::Zzzz;
    (void)script_id_from_name("Latn", &script);
    return script;
}

struct TextFixture {
    std::pmr::monotonic_buffer_resource memory;
    std::pmr::vector<DecodedCodePoint> codepoints{&memory};
    std::pmr::vector<GraphemeBoundary> graphemes{&memory};

    bool build() {
        constexpr std::string_view kText = "office affine a\xCC\x81";
        Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
        Utf8DecodeError decode_error;
        const auto bytes = std::as_bytes(std::span(kText.data(), kText.size()));
        if (!decoder.feed(bytes, 0U, &codepoints, &decode_error) ||
            !decoder.finish(&codepoints, &decode_error)) {
            return false;
        }
        GraphemeSegmentStats stats;
        GraphemeError error;
        return segment_graphemes(codepoints, &graphemes, &stats, &error);
    }
};

struct FontFixture {
    std::string identity;
    std::string family{"Prepared Shaping Family"};
    std::array<FontCoverageRange, 1> coverage{{{0x0020U, 0x007eU}}};
    std::array<FontDiscoveryFace, 1> faces;
    std::shared_ptr<const FontCatalogGeneration> generation;
    CatalogFontFaceBinding binding;
    std::shared_ptr<const PreparedHarfBuzzFace> prepared;

    bool build(
        const std::filesystem::path& path,
        std::size_t font_size,
        VerifiedFontResourceCache* cache) {
        identity = fontconfig_identity(path);
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
};

HarfBuzzShapingRequest base_request(const TextFixture& text) {
    HarfBuzzShapingRequest request;
    request.face_index = 0U;
    request.codepoints = text.codepoints;
    request.grapheme_boundaries = text.graphemes;
    request.first_cluster = 0U;
    request.cluster_limit =
        static_cast<std::uint32_t>(text.graphemes.size() - 1U);
    request.script = latin_script();
    request.direction = ShapingDirection::LeftToRight;
    request.language = "en";
    request.beginning_of_text = true;
    request.end_of_text = true;
    return request;
}

std::vector<ShapedGlyph> copy_glyphs(const ShapedGlyphRun& run) {
    return std::vector<ShapedGlyph>(run.glyphs.begin(), run.glyphs.end());
}

bool prepared_and_verified_are_equivalent(
    const FontFixture& font,
    const TextFixture& text) {
    ResourceLedger prepared_ledger;
    prepared_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource prepared_memory(
        prepared_ledger,
        ResourceClass::GlyphRun);
    ShapedGlyphRun prepared_output(&prepared_memory);
    HarfBuzzShapingStats prepared_stats;
    HarfBuzzShapingError prepared_error;
    HarfBuzzShapingRequest prepared_request = base_request(text);
    prepared_request.prepared_harfbuzz_face = font.prepared;

    bool ok = expect(
        shape_harfbuzz_segment(
            prepared_request,
            &prepared_output,
            &prepared_stats,
            &prepared_error),
        "prepared face must shape");
    ok &= expect(
        prepared_stats.used_prepared_harfbuzz_face &&
            prepared_stats.used_verified_font_resource &&
            !prepared_stats.performed_inline_font_verification &&
            prepared_stats.verified_font_resource_id ==
                font.prepared->resource_id() &&
            prepared_stats.glyph_count_before_shaping ==
                font.prepared->glyph_count() &&
            prepared_stats.units_per_em == font.prepared->units_per_em() &&
            !prepared_output.glyphs.empty(),
        "prepared path must publish exact native-face evidence");

    ResourceLedger verified_ledger;
    verified_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource verified_memory(
        verified_ledger,
        ResourceClass::GlyphRun);
    ShapedGlyphRun verified_output(&verified_memory);
    HarfBuzzShapingStats verified_stats;
    HarfBuzzShapingError verified_error;
    HarfBuzzShapingRequest verified_request = base_request(text);
    verified_request.verified_font_resource = font.binding.resource();
    ok &= expect(
        shape_harfbuzz_segment(
            verified_request,
            &verified_output,
            &verified_stats,
            &verified_error),
        "verified resource must shape");
    ok &= expect(
        !verified_stats.used_prepared_harfbuzz_face &&
            verified_stats.used_verified_font_resource &&
            copy_glyphs(prepared_output) == copy_glyphs(verified_output) &&
            prepared_output.x_scale == verified_output.x_scale &&
            prepared_output.y_scale == verified_output.y_scale,
        "prepared and verified paths must be byte-exact equivalents");
    return ok && prepared_ledger.accounting_clean() &&
        verified_ledger.accounting_clean();
}

bool per_call_font_state_is_isolated(
    const FontFixture& font,
    const TextFixture& text) {
    ResourceLedger first_ledger;
    first_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource first_memory(first_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun first_output(&first_memory);
    HarfBuzzShapingStats first_stats;
    HarfBuzzShapingError first_error;
    HarfBuzzShapingRequest first_request = base_request(text);
    first_request.prepared_harfbuzz_face = font.prepared;
    first_request.x_scale = 1000;
    first_request.y_scale = 1000;

    ResourceLedger second_ledger;
    second_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource second_memory(second_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun second_output(&second_memory);
    HarfBuzzShapingStats second_stats;
    HarfBuzzShapingError second_error;
    HarfBuzzShapingRequest second_request = base_request(text);
    second_request.prepared_harfbuzz_face = font.prepared;
    second_request.x_scale = 2000;
    second_request.y_scale = 2000;

    bool ok = expect(
        shape_harfbuzz_segment(
            first_request,
            &first_output,
            &first_stats,
            &first_error) &&
        shape_harfbuzz_segment(
            second_request,
            &second_output,
            &second_stats,
            &second_error),
        "two scale-isolated prepared calls must shape");
    ok &= expect(
        first_output.x_scale == 1000 && second_output.x_scale == 2000 &&
            first_output.glyphs.size() == second_output.glyphs.size() &&
            first_stats.total_x_advance != second_stats.total_x_advance &&
            font.prepared->units_per_em() > 0U,
        "per-call hb_font scale state must not mutate the immutable face");

    ResourceLedger repeat_ledger;
    repeat_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource repeat_memory(repeat_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun repeat_output(&repeat_memory);
    HarfBuzzShapingStats repeat_stats;
    HarfBuzzShapingError repeat_error;
    ok &= expect(
        shape_harfbuzz_segment(
            first_request,
            &repeat_output,
            &repeat_stats,
            &repeat_error) &&
            copy_glyphs(first_output) == copy_glyphs(repeat_output),
        "repeating the first scale after another call must be byte-exact");
    return ok && first_ledger.accounting_clean() &&
        second_ledger.accounting_clean() && repeat_ledger.accounting_clean();
}

bool invalid_modes_and_budget_are_atomic(
    const FontFixture& font,
    const TextFixture& text) {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource memory(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun output(&memory);
    HarfBuzzShapingStats stats;
    HarfBuzzShapingError error;
    HarfBuzzShapingRequest request = base_request(text);
    request.prepared_harfbuzz_face = font.prepared;
    bool ok = expect(
        shape_harfbuzz_segment(request, &output, &stats, &error),
        "failure fixture must seed output");

    request.verified_font_resource = font.binding.resource();
    ok &= expect(
        !shape_harfbuzz_segment(request, &output, &stats, &error) &&
            output.glyphs.empty() &&
            error.kind == HarfBuzzShapingErrorKind::InvalidInput,
        "prepared and verified inputs together must fail atomically");

    request.verified_font_resource.reset();
    request.face_index = 1U;
    ok &= expect(
        !shape_harfbuzz_segment(request, &output, &stats, &error) &&
            output.glyphs.empty() &&
            error.kind == HarfBuzzShapingErrorKind::InvalidInput,
        "prepared face-index mismatch must fail atomically");

    ResourceLedger tiny_ledger;
    tiny_ledger.set_hard_limit(ResourceClass::GlyphRun, 1U);
    LedgerMemoryResource tiny_memory(tiny_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun tiny_output(&tiny_memory);
    HarfBuzzShapingStats tiny_stats;
    HarfBuzzShapingError tiny_error;
    request = base_request(text);
    request.prepared_harfbuzz_face = font.prepared;
    ok &= expect(
        !shape_harfbuzz_segment(
            request,
            &tiny_output,
            &tiny_stats,
            &tiny_error) &&
            tiny_output.glyphs.empty() &&
            tiny_error.kind ==
                HarfBuzzShapingErrorKind::OutputBudgetExceeded,
        "one-byte prepared glyph budget must fail atomically");
    return ok && ledger.accounting_clean() && tiny_ledger.accounting_clean();
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
    FontFixture font;
    TextFixture text;
    if (!font.build(font_path, static_cast<std::size_t>(file_size), &cache) ||
        !text.build()) {
        return 2;
    }
    cache.clear();
    font.generation.reset();
    font.binding = CatalogFontFaceBinding{};

    bool ok = true;
    ok &= prepared_and_verified_are_equivalent(font, text);
    ok &= per_call_font_state_is_isolated(font, text);
    ok &= invalid_modes_and_budget_are_atomic(font, text);
    if (!ok) {
        return 1;
    }
    std::cout << "prepared HarfBuzz shaping tests passed\n";
    return 0;
}
