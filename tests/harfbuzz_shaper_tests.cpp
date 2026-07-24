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
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace zevryon::core;
using namespace zevryon::text;

constexpr std::uint32_t tag(char a, char b, char c, char d) noexcept {
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
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        std::cerr << "unable to open font: " << path << '\n';
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
    }
    return script;
}

bool shape(
    std::span<const std::byte> font,
    TextFixture& text,
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
            text.codepoints,
            text.graphemes,
            0U,
            static_cast<std::uint32_t>(text.graphemes.size() - 1U),
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

bool validate_run(
    const ShapedGlyphRun& run,
    const HarfBuzzShapingStats& stats,
    std::uint32_t cluster_limit) {
    bool ok = true;
    ok &= expect(!run.glyphs.empty(), "shaping must emit glyphs");
    ok &= expect(
        run.glyphs.size() == stats.output_glyphs,
        "glyph vector and stats must agree");
    ok &= expect(
        run.first_cluster == 0U && run.cluster_limit == cluster_limit,
        "run metadata must preserve the cluster range");
    ok &= expect(stats.units_per_em != 0U, "units-per-em must be recorded");
    for (const ShapedGlyph glyph : run.glyphs) {
        ok &= expect(
            glyph.cluster_index < cluster_limit,
            "output cluster must remain inside the request");
    }
    return ok;
}

bool test_latin(std::span<const std::byte> font) {
    TextFixture text;
    if (!make_text("office a\xCC\x81", &text)) {
        return false;
    }
    const std::array<ShapingFeature, 1> liga_on{{
        {tag('l', 'i', 'g', 'a'), 1U}}};
    const std::array<ShapingFeature, 1> liga_off{{
        {tag('l', 'i', 'g', 'a'), 0U}}};

    ResourceLedger first_ledger;
    first_ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource first_resource(first_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun first(&first_resource);
    HarfBuzzShapingStats first_stats;
    HarfBuzzShapingError first_error;
    if (!shape(
            font,
            text,
            require_script("Latn"),
            ShapingDirection::LeftToRight,
            "en",
            liga_on,
            &first,
            &first_stats,
            &first_error)) {
        std::cerr << "Latin shaping failed: " << first_error.message << '\n';
        return false;
    }

    ResourceLedger repeat_ledger;
    repeat_ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource repeat_resource(repeat_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun repeat(&repeat_resource);
    HarfBuzzShapingStats repeat_stats;
    HarfBuzzShapingError repeat_error;
    if (!shape(
            font,
            text,
            require_script("Latn"),
            ShapingDirection::LeftToRight,
            "en",
            liga_on,
            &repeat,
            &repeat_stats,
            &repeat_error)) {
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
            text,
            require_script("Latn"),
            ShapingDirection::LeftToRight,
            "en",
            liga_off,
            &disabled,
            &disabled_stats,
            &disabled_error)) {
        return false;
    }

    const std::uint32_t cluster_limit = static_cast<std::uint32_t>(
        text.graphemes.size() - 1U);
    bool ok = validate_run(first, first_stats, cluster_limit);
    ok &= expect(first.glyphs == repeat.glyphs, "repeat shaping must be byte-exact");
    ok &= expect(
        first_stats.total_x_advance == repeat_stats.total_x_advance &&
            first_stats.output_glyphs == repeat_stats.output_glyphs,
        "repeat shaping stats must be deterministic");
    ok &= expect(
        first.glyphs.size() <= disabled.glyphs.size(),
        "standard ligatures must not increase glyph count");
    ok &= expect(first_stats.missing_glyphs == 0U, "Latin fixture must be covered");
    ok &= expect(
        first_ledger.snapshot(ResourceClass::GlyphRun).current_bytes ==
            first.glyphs.size() * sizeof(ShapedGlyph),
        "glyph output must use one exact accounted reserve");
    ok &= expect(first_ledger.accounting_clean(), "Latin accounting must be clean");
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
            text,
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
        static_cast<std::uint32_t>(text.graphemes.size() - 1U));
    ok &= expect(stats.missing_glyphs == 0U, "RTL fixture must be covered");
    for (std::size_t index = 1U; index < run.glyphs.size(); ++index) {
        ok &= expect(
            run.glyphs[index - 1U].cluster_index >=
                run.glyphs[index].cluster_index,
            "RTL clusters must remain monotone decreasing");
    }
    return ok && ledger.accounting_clean();
}

bool test_devanagari(std::span<const std::byte> font) {
    TextFixture text;
    if (!make_text(
            "\xE0\xA4\x95\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\xAE",
            &text)) {
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
            text,
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
    return validate_run(
               run,
               stats,
               static_cast<std::uint32_t>(text.graphemes.size() - 1U)) &&
           expect(stats.missing_glyphs == 0U, "Devanagari fixture must be covered") &&
           ledger.accounting_clean();
}

bool test_failures(std::span<const std::byte> font) {
    TextFixture text;
    if (!make_text("budget", &text)) {
        return false;
    }

    ResourceLedger tiny_ledger;
    tiny_ledger.set_hard_limit(ResourceClass::GlyphRun, 1U);
    LedgerMemoryResource tiny_resource(tiny_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun tiny(&tiny_resource);
    HarfBuzzShapingStats tiny_stats;
    HarfBuzzShapingError tiny_error;
    bool ok = expect(
        !shape(
            font,
            text,
            require_script("Latn"),
            ShapingDirection::LeftToRight,
            "en",
            {},
            &tiny,
            &tiny_stats,
            &tiny_error),
        "one-byte glyph budget must fail");
    ok &= expect(
        tiny_error.kind == HarfBuzzShapingErrorKind::OutputBudgetExceeded,
        "glyph budget failure must report exact error kind");
    ok &= expect(tiny.glyphs.empty(), "budget failure must publish no glyphs");
    ok &= expect(
        tiny_ledger.snapshot(ResourceClass::GlyphRun).rejected_reservations > 0U,
        "glyph budget rejection must be accounted");

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
            text,
            require_script("Latn"),
            ShapingDirection::LeftToRight,
            "en",
            {},
            &invalid,
            &invalid_stats,
            &invalid_error),
        "invalid font bytes must fail");
    ok &= expect(
        invalid_error.kind == HarfBuzzShapingErrorKind::InvalidFontData,
        "invalid font bytes must report exact error kind");
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
    ok &= test_latin(latin_font);
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
