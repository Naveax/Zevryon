#include "grapheme_segmenter.hpp"
#include "harfbuzz_shaper.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_script.hpp"
#include "unicode_stream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace zevryon::core;
using namespace zevryon::text;

constexpr std::uint32_t tag(
    char a,
    char b,
    char c,
    char d) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

struct TextFixture {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::vector<DecodedCodePoint> codepoints{&resource};
    std::pmr::vector<GraphemeBoundary> graphemes{&resource};
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool load_file(const char* path, std::vector<std::byte>* output) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        std::cerr << "unable to open font: " << path << '\n';
        return false;
    }
    const std::vector<char> bytes(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
    output->resize(bytes.size());
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        (*output)[index] = static_cast<std::byte>(
            static_cast<unsigned char>(bytes[index]));
    }
    return !output->empty();
}

bool make_text(std::string_view utf8, TextFixture* fixture) {
    Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
    Utf8DecodeError decode_error;
    const auto bytes = std::as_bytes(std::span(utf8.data(), utf8.size()));
    if (!decoder.feed(bytes, 0U, &fixture->codepoints, &decode_error) ||
        !decoder.finish(&fixture->codepoints, &decode_error)) {
        std::cerr << "decode failed: " << decode_error.message << '\n';
        return false;
    }
    GraphemeSegmentStats stats;
    GraphemeError grapheme_error;
    if (!segment_graphemes(
            fixture->codepoints,
            &fixture->graphemes,
            &stats,
            &grapheme_error)) {
        std::cerr << "grapheme segmentation failed: "
                  << grapheme_error.message << '\n';
        return false;
    }
    return !fixture->codepoints.empty() && fixture->graphemes.size() >= 2U;
}

ScriptId require_script(std::string_view name) {
    ScriptId script = ScriptId::Zzzz;
    if (!script_id_from_name(name, &script)) {
        std::cerr << "missing Script: " << name << '\n';
        return ScriptId::Zzzz;
    }
    return script;
}

bool validate_run(
    const ShapedGlyphRun& run,
    const HarfBuzzShapingStats& stats,
    std::uint32_t first_cluster,
    std::uint32_t cluster_limit) {
    bool ok = true;
    ok &= expect(!run.glyphs.empty(), "shaping must emit glyphs");
    ok &= expect(
        run.glyphs.size() == stats.output_glyphs,
        "glyph vector and stats must agree");
    ok &= expect(
        run.first_cluster == first_cluster &&
            run.cluster_limit == cluster_limit,
        "run metadata must preserve the requested cluster range");
    ok &= expect(stats.units_per_em != 0U, "font units-per-em must be recorded");
    for (const ShapedGlyph glyph : run.glyphs) {
        ok &= expect(
            glyph.cluster_index >= first_cluster &&
                glyph.cluster_index < cluster_limit,
            "every output cluster must remain within the request");
    }
    return ok;
}

bool shape(
    std::span<const std::byte> font,
    TextFixture* text,
    ScriptId script,
    ShapingDirection direction,
    std::string_view language,
    std::span<const ShapingFeature> features,
    ShapedGlyphRun* output,
    HarfBuzzShapingStats* stats,
    HarfBuzzShapingError* error) {
    return shape_harfbuzz_segment(
        HarfBuzzShapingRequest{
            font,
            0U,
            text->codepoints,
            text->graphemes,
            0U,
            static_cast<std::uint32_t>(text->graphemes.size() - 1U),
            script,
            direction,
            language,
            features,
            {},
            0,
            0,
            true,
            true,
            true},
        output,
        stats,
        error);
}

bool test_latin_and_determinism(std::span<const std::byte> font) {
    TextFixture text;
    if (!make_text("office a\xCC\x81", &text)) {
        return false;
    }
    const ScriptId latin = require_script("Latn");
    const std::array<ShapingFeature, 1> liga_on{{{tag('l', 'i', 'g', 'a'), 1U}}};
    const std::array<ShapingFeature, 1> liga_off{{{tag('l', 'i', 'g', 'a'), 0U}}};

    ResourceLedger first_ledger;
    first_ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource first_resource(first_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun first(&first_resource);
    HarfBuzzShapingStats first_stats;
    HarfBuzzShapingError first_error;
    if (!shape(
            font,
            &text,
            latin,
            ShapingDirection::LeftToRight,
            "en",
            liga_on,
            &first,
            &first_stats,
            &first_error)) {
        std::cerr << "Latin shaping failed: " << first_error.message << '\n';
        return false;
    }

    ResourceLedger second_ledger;
    second_ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource second_resource(second_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun second(&second_resource);
    HarfBuzzShapingStats second_stats;
    HarfBuzzShapingError second_error;
    if (!shape(
            font,
            &text,
            latin,
            ShapingDirection::LeftToRight,
            "en",
            liga_on,
            &second,
            &second_stats,
            &second_error)) {
        std::cerr << "repeat Latin shaping failed: "
                  << second_error.message << '\n';
        return false;
    }

    ResourceLedger disabled_ledger;
    disabled_ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource disabled_resource(disabled_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun disabled(&disabled_resource);
    HarfBuzzShapingStats disabled_stats;
    HarfBuzzShapingError disabled_error;
    if (!shape(
            font,
            &text,
            latin,
            ShapingDirection::LeftToRight,
            "en",
            liga_off,
            &disabled,
            &disabled_stats,
            &disabled_error)) {
        std::cerr << "Latin shaping without liga failed: "
                  << disabled_error.message << '\n';
        return false;
    }

    bool ok = validate_run(
        first,
        first_stats,
        0U,
        static_cast<std::uint32_t>(text.graphemes.size() - 1U));
    ok &= expect(first.glyphs == second.glyphs, "repeated shaping must be byte-exact");
    ok &= expect(
        first_stats.output_glyphs == second_stats.output_glyphs &&
            first_stats.total_x_advance == second_stats.total_x_advance &&
            first_stats.total_y_advance == second_stats.total_y_advance,
        "repeated shaping stats must remain deterministic");
    ok &= expect(
        first.glyphs.size() <= disabled.glyphs.size(),
        "enabling standard ligatures must not increase Latin glyph count");
    ok &= expect(first_stats.missing_glyphs == 0U, "Latin font must cover fixture");
    ok &= expect(
        first_ledger.snapshot(ResourceClass::GlyphRun).current_bytes ==
            first.glyphs.size() * sizeof(ShapedGlyph),
        "one exact output reserve must account every shaped glyph byte");
    ok &= expect(first_ledger.accounting_clean(), "Latin shaping accounting must be clean");
    return ok;
}

bool test_rtl(
    std::span<const std::byte> font,
    std::string_view utf8,
    std::string_view script_name,
    std::string_view language) {
    TextFixture text;
    if (!make_text(utf8, &text)) {
        return false;
    }
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource resource(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun run(&resource);
    HarfBuzzShapingStats stats;
    HarfBuzzShapingError error;
    if (!shape(
            font,
            &text,
            require_script(script_name),
            ShapingDirection::RightToLeft,
            language,
            {},
            &run,
            &stats,
            &error)) {
        std::cerr << script_name << " shaping failed: " << error.message << '\n';
        return false;
    }

    bool ok = validate_run(
        run,
        stats,
        0U,
        static_cast<std::uint32_t>(text.graphemes.size() - 1U));
    ok &= expect(stats.missing_glyphs == 0U, "RTL font must cover fixture");
    for (std::size_t index = 1U; index < run.glyphs.size(); ++index) {
        ok &= expect(
            run.glyphs[index - 1U].cluster_index >=
                run.glyphs[index].cluster_index,
            "RTL monotone-grapheme clusters must not increase");
    }
    ok &= expect(ledger.accounting_clean(), "RTL shaping accounting must be clean");
    return ok;
}

bool test_devanagari(std::span<const std::byte> font) {
    TextFixture text;
    if (!make_text("\xE0\xA4\x95\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\xAE", &text)) {
        return false;
    }
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource resource(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun run(&resource);
    HarfBuzzShapingStats stats;
    HarfBuzzShapingError error;
    if (!shape(
            font,
            &text,
            require_script("Deva"),
            ShapingDirection::LeftToRight,
            "hi",
            {},
            &run,
            &stats,
            &error)) {
        std::cerr << "Devanagari shaping failed: " << error.message << '\n';
        return false;
    }
    bool ok = validate_run(
        run,
        stats,
        0U,
        static_cast<std::uint32_t>(text.graphemes.size() - 1U));
    ok &= expect(stats.missing_glyphs == 0U, "Devanagari font must cover fixture");
    ok &= expect(ledger.accounting_clean(), "Devanagari accounting must be clean");
    return ok;
}

bool test_failures(std::span<const std::byte> font) {
    TextFixture text;
    if (!make_text("budget", &text)) {
        return false;
    }
    const ScriptId latin = require_script("Latn");

    ResourceLedger tiny_ledger;
    tiny_ledger.set_hard_limit(ResourceClass::GlyphRun, 1U);
    LedgerMemoryResource tiny_resource(tiny_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun tiny(&tiny_resource);
    HarfBuzzShapingStats tiny_stats;
    HarfBuzzShapingError tiny_error;
    bool ok = expect(
        !shape(
            font,
            &text,
            latin,
            ShapingDirection::LeftToRight,
            "en",
            {},
            &tiny,
            &tiny_stats,
            &tiny_error),
        "tiny glyph budget must fail");
    ok &= expect(
        tiny_error.kind == HarfBuzzShapingErrorKind::OutputBudgetExceeded,
        "tiny glyph budget must report output budget failure");
    ok &= expect(tiny.glyphs.empty(), "budget failure must publish no glyphs");
    ok &= expect(
        tiny_ledger.snapshot(ResourceClass::GlyphRun).rejected_reservations > 0U,
        "budget rejection must be accounted");

    const std::array<std::byte, 8> invalid_font{};
    ResourceLedger invalid_ledger;
    invalid_ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U);
    LedgerMemoryResource invalid_resource(invalid_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun invalid(&invalid_resource);
    HarfBuzzShapingStats invalid_stats;
    HarfBuzzShapingError invalid_error;
    ok &= expect(
        !shape(
            invalid_font,
            &text,
            latin,
            ShapingDirection::LeftToRight,
            "en",
            {},
            &invalid,
            &invalid_stats,
            &invalid_error),
        "invalid font data must fail");
    ok &= expect(
        invalid_error.kind == HarfBuzzShapingErrorKind::InvalidFontData,
        "invalid font data must report exact error kind");
    ok &= expect(invalid.glyphs.empty(), "invalid font must publish no glyphs");
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: harfbuzz_shaper_tests LATIN_FONT DEVANAGARI_FONT\n";
        return 2;
    }

    std::vector<std::byte> latin_font;
    std::vector<std::byte> devanagari_font;
    if (!load_file(argv[1], &latin_font) ||
        !load_file(argv[2], &devanagari_font)) {
        return 2;
    }

    bool ok = true;
    ok &= test_latin_and_determinism(latin_font);
    ok &= test_rtl(
        latin_font,
        "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85",
        "Arab",
        "ar");
    ok &= test_rtl(
        latin_font,
        "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D",
        "Hebr",
        "he");
    ok &= test_devanagari(devanagari_font);
    ok &= test_failures(latin_font);

    if (!ok) {
        return 1;
    }
    std::cout << "bounded HarfBuzz shaping tests passed\n";
    return 0;
}
