#include "grapheme_segmenter.hpp"
#include "harfbuzz_shaper.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_script.hpp"
#include "unicode_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
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
    constexpr std::string_view kText = "office";
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

HarfBuzzShapingRequest make_request(
    std::span<const std::byte> font,
    std::uint32_t face_index,
    const TextFixture& text) {
    ScriptId latin = ScriptId::Zzzz;
    (void)script_id_from_name("Latn", &latin);
    HarfBuzzShapingRequest request;
    request.font_bytes = font;
    request.face_index = face_index;
    request.codepoints = text.codepoints;
    request.grapheme_boundaries = text.graphemes;
    request.first_cluster = 0U;
    request.cluster_limit =
        static_cast<std::uint32_t>(text.graphemes.size() - 1U);
    request.script = latin;
    request.direction = ShapingDirection::LeftToRight;
    request.language = "en";
    request.beginning_of_text = true;
    request.end_of_text = true;
    return request;
}

bool valid_font_is_verified(
    const std::vector<std::byte>& font,
    const TextFixture& text) {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource resource(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun output(&resource);
    HarfBuzzShapingStats stats;
    HarfBuzzShapingError error;
    const bool shaped = shape_harfbuzz_segment(
        make_request(font, 0U, text),
        &output,
        &stats,
        &error);
    bool ok = expect(shaped, "valid font must shape after strict verification");
    ok &= expect(!output.glyphs.empty(), "verified shaping must publish glyphs");
    ok &= expect(stats.validated_font_faces == 1U,
                 "single-font validation must report one face");
    ok &= expect(stats.validated_font_tables != 0U,
                 "font table count must be recorded");
    ok &= expect(stats.verified_font_table_checksums ==
                     stats.validated_font_tables,
                 "every validated table checksum must be verified");
    ok &= expect(stats.verified_font_payload_bytes != 0U,
                 "verified font payload bytes must be recorded");
    ok &= expect(stats.whole_font_checksum_verified,
                 "standalone font whole checksum must be verified");
    ok &= expect(error.sfnt_parse_error == SfntParseErrorKind::None &&
                     error.sfnt_integrity_error ==
                         SfntIntegrityErrorKind::None,
                 "successful shaping must expose no font sub-error");
    return ok && ledger.accounting_clean();
}

bool corrupted_table_is_rejected_before_backend(
    const std::vector<std::byte>& font,
    const TextFixture& text) {
    SfntResourceView view;
    if (!open_sfnt_resource(font, 0U, &view, nullptr, nullptr)) {
        return false;
    }
    SfntTableRecord record;
    if (!view.find_table(sfnt_tag('c', 'm', 'a', 'p'), &record) ||
        record.length == 0U) {
        return false;
    }
    std::vector<std::byte> corrupted = font;
    corrupted[record.offset] ^= std::byte{1U};

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource resource(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun output(&resource);
    HarfBuzzShapingStats stats;
    HarfBuzzShapingError error;
    bool ok = expect(
        !shape_harfbuzz_segment(
            make_request(corrupted, 0U, text),
            &output,
            &stats,
            &error),
        "corrupted font table must fail before HarfBuzz shaping");
    ok &= expect(error.kind == HarfBuzzShapingErrorKind::InvalidFontData,
                 "font integrity failure must map to InvalidFontData");
    ok &= expect(error.sfnt_parse_error == SfntParseErrorKind::None &&
                     error.sfnt_integrity_error ==
                         SfntIntegrityErrorKind::TableChecksumMismatch,
                 "corrupted table must expose exact integrity sub-error");
    ok &= expect(error.font_table_tag == sfnt_tag('c', 'm', 'a', 'p') &&
                     error.input_index == record.offset,
                 "integrity error must preserve table tag and byte offset");
    ok &= expect(output.glyphs.empty() && stats.output_glyphs == 0U,
                 "rejected font must publish no backend output or stats");
    return ok && ledger.accounting_clean();
}

bool structural_failures_are_exact(
    const std::vector<std::byte>& font,
    const TextFixture& text) {
    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, 1024U * 1024U);
    LedgerMemoryResource resource(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun output(&resource);
    HarfBuzzShapingStats stats;
    HarfBuzzShapingError error;

    const std::vector<std::byte> invalid(12U, std::byte{0});
    bool ok = expect(
        !shape_harfbuzz_segment(
            make_request(invalid, 0U, text),
            &output,
            &stats,
            &error),
        "non-SFNT bytes must fail structural validation");
    ok &= expect(error.kind == HarfBuzzShapingErrorKind::InvalidFontData &&
                     error.sfnt_parse_error ==
                         SfntParseErrorKind::UnsupportedSfntVersion,
                 "non-SFNT bytes must expose exact parse sub-error");
    ok &= expect(error.sfnt_integrity_error == SfntIntegrityErrorKind::None &&
                     output.glyphs.empty(),
                 "structural failure must not enter integrity or backend stages");

    ok &= expect(
        !shape_harfbuzz_segment(
            make_request(font, 1U, text),
            &output,
            &stats,
            &error),
        "out-of-range face index must fail before backend creation");
    ok &= expect(error.sfnt_parse_error == SfntParseErrorKind::InvalidFaceIndex &&
                     error.input_index == 0U,
                 "single-font face-index failure must preserve parser result");
    return ok && ledger.accounting_clean();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: harfbuzz_verified_input_tests FONT\n";
        return 2;
    }
    std::vector<std::byte> font;
    TextFixture text;
    if (!load_file(argv[1], &font) || !make_text(&text)) {
        return 2;
    }

    bool ok = true;
    ok &= valid_font_is_verified(font, text);
    ok &= corrupted_table_is_rejected_before_backend(font, text);
    ok &= structural_failures_are_exact(font, text);
    if (!ok) {
        return 1;
    }
    std::cout << "verified HarfBuzz font-input tests passed\n";
    return 0;
}
