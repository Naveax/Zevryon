#include "grapheme_segmenter.hpp"
#include "harfbuzz_shaper.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_script.hpp"
#include "unicode_stream.hpp"
#include "verified_font_resource.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace zevryon::core;
using namespace zevryon::text;

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
    stream.seekg(0U, std::ios::beg);
    stream.read(
        reinterpret_cast<char*>(output->data()),
        static_cast<std::streamsize>(output->size()));
    return stream.good() || stream.eof();
}

bool make_text(TextFixture* fixture) {
    constexpr std::string_view kText = "office a\xCC\x81";
    Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
    Utf8DecodeError decode_error;
    const auto bytes = std::as_bytes(std::span(kText.data(), kText.size()));
    if (!decoder.feed(bytes, 0U, &fixture->codepoints, &decode_error) ||
        !decoder.finish(&fixture->codepoints, &decode_error)) {
        return false;
    }
    GraphemeSegmentStats stats;
    GraphemeError error;
    return segment_graphemes(
        fixture->codepoints,
        &fixture->graphemes,
        &stats,
        &error);
}

ScriptId latin_script() {
    ScriptId script = ScriptId::Zzzz;
    (void)script_id_from_name("Latn", &script);
    return script;
}

HarfBuzzShapingRequest make_request(const TextFixture& text) {
    HarfBuzzShapingRequest request;
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

bool shape_raw(
    const TextFixture& text,
    std::span<const std::byte> font,
    ShapedGlyphRun* output,
    HarfBuzzShapingStats* stats,
    HarfBuzzShapingError* error) {
    HarfBuzzShapingRequest request = make_request(text);
    request.font_bytes = font;
    return shape_harfbuzz_segment(request, output, stats, error);
}

bool shape_retained(
    const TextFixture& text,
    const std::shared_ptr<const VerifiedFontResource>& resource,
    ShapedGlyphRun* output,
    HarfBuzzShapingStats* stats,
    HarfBuzzShapingError* error) {
    HarfBuzzShapingRequest request = make_request(text);
    request.face_index = resource->view().face_index();
    request.verified_font_resource = resource;
    return shape_harfbuzz_segment(request, output, stats, error);
}

bool output_equivalence_and_lifetime(
    std::vector<std::byte> font,
    const TextFixture& text) {
    ResourceLedger raw_ledger;
    raw_ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource raw_memory(raw_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun raw_output(&raw_memory);
    HarfBuzzShapingStats raw_stats;
    HarfBuzzShapingError raw_error;
    bool ok = expect(
        shape_raw(text, font, &raw_output, &raw_stats, &raw_error),
        "raw verified path must shape the real font");
    ok &= expect(raw_stats.performed_inline_font_verification &&
                     !raw_stats.used_verified_font_resource &&
                     raw_stats.verified_font_resource_id == 0U,
                 "raw path statistics must report inline verification");

    std::shared_ptr<const VerifiedFontResource> resource;
    VerifiedFontResourceStats resource_stats;
    VerifiedFontResourceError resource_error;
    ok &= expect(
        build_verified_font_resource(
            9001U,
            font,
            0U,
            font.size(),
            &resource,
            &resource_stats,
            &resource_error),
        "real font must build one immutable verified resource");
    if (!resource) {
        return false;
    }

    font.clear();
    font.shrink_to_fit();

    ResourceLedger retained_ledger;
    retained_ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource retained_memory(
        retained_ledger,
        ResourceClass::GlyphRun);
    ShapedGlyphRun retained_output(&retained_memory);
    HarfBuzzShapingStats retained_stats;
    HarfBuzzShapingError retained_error;
    ok &= expect(
        shape_retained(
            text,
            resource,
            &retained_output,
            &retained_stats,
            &retained_error),
        "immutable resource must shape after caller bytes are destroyed");
    ok &= expect(
        std::vector<ShapedGlyph>(raw_output.glyphs.begin(), raw_output.glyphs.end()) ==
            std::vector<ShapedGlyph>(
                retained_output.glyphs.begin(), retained_output.glyphs.end()),
        "raw and retained paths must produce byte-equivalent glyph records");
    ok &= expect(retained_stats.used_verified_font_resource &&
                     !retained_stats.performed_inline_font_verification &&
                     retained_stats.verified_font_resource_id == 9001U,
                 "retained path statistics must identify the immutable resource");
    ok &= expect(retained_stats.validated_font_tables ==
                     resource_stats.parse.table_count &&
                     retained_stats.verified_font_table_checksums ==
                         resource_stats.integrity.checksums_verified &&
                     retained_stats.output_glyphs == raw_stats.output_glyphs,
                 "retained validation evidence and shaping counts must be reused");
    ok &= expect(raw_ledger.accounting_clean() &&
                     retained_ledger.accounting_clean() &&
                     resource->accounting_clean(),
                 "raw, retained, and font-resource ledgers must remain clean");
    return ok;
}

bool ambiguous_and_mismatched_requests_fail(
    const std::vector<std::byte>& font,
    const TextFixture& text) {
    std::shared_ptr<const VerifiedFontResource> resource;
    if (!build_verified_font_resource(
            44U,
            font,
            0U,
            font.size(),
            &resource,
            nullptr,
            nullptr)) {
        return false;
    }

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource memory(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun output(&memory);
    HarfBuzzShapingStats stats;
    HarfBuzzShapingError error;
    bool ok = expect(
        shape_retained(text, resource, &output, &stats, &error),
        "failure fixture must first publish glyphs");
    ok &= expect(!output.glyphs.empty(),
                 "failure fixture must contain prior output");

    HarfBuzzShapingRequest ambiguous = make_request(text);
    ambiguous.font_bytes = font;
    ambiguous.verified_font_resource = resource;
    ok &= expect(
        !shape_harfbuzz_segment(ambiguous, &output, &stats, &error),
        "raw bytes and immutable handle together must fail");
    ok &= expect(error.kind == HarfBuzzShapingErrorKind::InvalidInput &&
                     output.glyphs.empty() && stats.output_glyphs == 0U,
                 "ambiguous input must reset output and publish no stats");

    HarfBuzzShapingRequest mismatch = make_request(text);
    mismatch.face_index = 1U;
    mismatch.verified_font_resource = resource;
    ok &= expect(
        !shape_harfbuzz_segment(mismatch, &output, &stats, &error),
        "resource face mismatch must fail before backend creation");
    ok &= expect(error.kind == HarfBuzzShapingErrorKind::InvalidInput &&
                     error.input_index == 1U && output.glyphs.empty(),
                 "face mismatch must preserve requested face and atomic output");
    return ok && ledger.accounting_clean();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: harfbuzz_verified_resource_tests FONT\n";
        return 2;
    }
    std::vector<std::byte> font;
    TextFixture text;
    if (!load_file(argv[1], &font) || !make_text(&text)) {
        return 2;
    }

    bool ok = true;
    ok &= output_equivalence_and_lifetime(font, text);
    ok &= ambiguous_and_mismatched_requests_fail(font, text);
    if (!ok) {
        return 1;
    }
    std::cout << "verified HarfBuzz resource-handle tests passed\n";
    return 0;
}
