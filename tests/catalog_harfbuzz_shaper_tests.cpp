#include "catalog_harfbuzz_shaper.hpp"

#include "grapheme_segmenter.hpp"
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
#include <utility>
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
    append_field(&result, "CatalogBridgePS");
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
        constexpr std::string_view kText = "office a\xCC\x81";
        Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
        Utf8DecodeError decode_error;
        const auto bytes = std::as_bytes(std::span(kText.data(), kText.size()));
        if (!decoder.feed(bytes, 0U, &codepoints, &decode_error) ||
            !decoder.finish(&codepoints, &decode_error)) {
            return false;
        }
        GraphemeSegmentStats stats;
        GraphemeError error;
        return segment_graphemes(
            codepoints,
            &graphemes,
            &stats,
            &error);
    }
};

struct GenerationFixture {
    std::string identity;
    std::string family{"Catalog Bridge Family"};
    std::array<FontCoverageRange, 1> coverage{{{0x0020U, 0x007eU}}};
    std::array<FontDiscoveryFace, 1> faces;
    std::shared_ptr<const FontCatalogGeneration> generation;

    bool build(const std::filesystem::path& path, std::uint64_t generation_id) {
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
        FontDiscoveryStats stats;
        FontDiscoveryError error;
        return build_font_catalog_generation(
            generation_id,
            faces,
            kDiscoveryLimit,
            kCatalogLimit,
            &generation,
            &stats,
            &error);
    }
};

CatalogHarfBuzzShapingRequest make_catalog_request(
    const GenerationFixture& generation,
    const TextFixture& text,
    VerifiedFontResourceCache* cache,
    std::size_t staging_limit) {
    CatalogHarfBuzzShapingRequest request;
    request.generation = generation.generation;
    request.face_id = 0U;
    request.staging_hard_limit = staging_limit;
    request.cache = cache;
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

HarfBuzzShapingRequest make_direct_request(
    const TextFixture& text,
    std::shared_ptr<const VerifiedFontResource> resource) {
    HarfBuzzShapingRequest request;
    request.face_index = resource->view().face_index();
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
    request.verified_font_resource = std::move(resource);
    return request;
}

bool success_cache_and_equivalence(
    const std::filesystem::path& path,
    std::size_t font_size,
    const TextFixture& text) {
    GenerationFixture generation;
    if (!generation.build(path, 710U)) {
        return false;
    }
    VerifiedFontResourceCache cache(font_size * 3U, 64U * 1024U, 4U);
    const CatalogHarfBuzzShapingRequest request =
        make_catalog_request(generation, text, &cache, font_size * 2U);

    ResourceLedger first_ledger;
    first_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource first_memory(first_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun first_output(&first_memory);
    CatalogHarfBuzzShapingStats first_stats;
    CatalogHarfBuzzShapingError first_error;
    bool ok = expect(
        shape_catalog_harfbuzz_segment(
            request,
            &first_output,
            &first_stats,
            &first_error),
        "catalog face must resolve and shape");
    ok &= expect(
        first_stats.resource_resolved && first_stats.shaping_completed &&
            first_stats.resource.generation_id == 710U &&
            first_stats.resource.face_id == 0U &&
            first_stats.resource.file_load.cache.build_attempts == 1U &&
            first_stats.resource.file_load.cache.builds_published == 1U &&
            first_stats.shaping.used_verified_font_resource &&
            !first_stats.shaping.performed_inline_font_verification &&
            !first_output.glyphs.empty(),
        "first catalog shape must publish exact resolver and retained-shaping evidence");

    ResourceLedger second_ledger;
    second_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource second_memory(second_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun second_output(&second_memory);
    CatalogHarfBuzzShapingStats second_stats;
    CatalogHarfBuzzShapingError second_error;
    ok &= expect(
        shape_catalog_harfbuzz_segment(
            request,
            &second_output,
            &second_stats,
            &second_error),
        "repeated catalog shape must succeed");
    ok &= expect(
        second_stats.resource.file_load.cache.hits >= 1U &&
            second_stats.resource.file_load.cache.build_attempts == 1U &&
            std::vector<ShapedGlyph>(
                first_output.glyphs.begin(), first_output.glyphs.end()) ==
                std::vector<ShapedGlyph>(
                    second_output.glyphs.begin(), second_output.glyphs.end()),
        "repeated catalog shape must reuse the resource and preserve glyph bytes");

    std::shared_ptr<const VerifiedFontResource> resource;
    CatalogFontResourceStats resource_stats;
    CatalogFontResourceError resource_error;
    ok &= expect(
        resolve_catalog_font_resource(
            generation.generation,
            0U,
            font_size * 2U,
            &cache,
            &resource,
            &resource_stats,
            &resource_error),
        "direct equivalence fixture must resolve the resident resource");

    ResourceLedger direct_ledger;
    direct_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource direct_memory(direct_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun direct_output(&direct_memory);
    HarfBuzzShapingStats direct_stats;
    HarfBuzzShapingError direct_error;
    ok &= expect(
        resource && shape_harfbuzz_segment(
                        make_direct_request(text, resource),
                        &direct_output,
                        &direct_stats,
                        &direct_error),
        "direct retained-resource path must shape");
    ok &= expect(
        std::vector<ShapedGlyph>(
            first_output.glyphs.begin(), first_output.glyphs.end()) ==
            std::vector<ShapedGlyph>(
                direct_output.glyphs.begin(), direct_output.glyphs.end()) &&
            direct_stats.verified_font_resource_id ==
                first_stats.shaping.verified_font_resource_id,
        "catalog bridge and direct retained path must be byte-exact equivalents");
    ok &= expect(
        first_ledger.accounting_clean() && second_ledger.accounting_clean() &&
            direct_ledger.accounting_clean(),
        "all glyph output ledgers must remain clean");
    return ok;
}

bool nested_failures_clear_output(
    const std::filesystem::path& path,
    std::size_t font_size,
    const TextFixture& text) {
    GenerationFixture generation;
    if (!generation.build(path, 711U)) {
        return false;
    }
    VerifiedFontResourceCache cache(font_size * 3U, 64U * 1024U, 4U);
    CatalogHarfBuzzShapingRequest request =
        make_catalog_request(generation, text, &cache, font_size * 2U);

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource memory(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun output(&memory);
    CatalogHarfBuzzShapingStats stats;
    CatalogHarfBuzzShapingError error;
    bool ok = expect(
        shape_catalog_harfbuzz_segment(request, &output, &stats, &error),
        "failure test must first seed glyph output");

    request.face_id = kInvalidFontFaceId;
    ok &= expect(
        !shape_catalog_harfbuzz_segment(request, &output, &stats, &error),
        "invalid catalog face must fail");
    ok &= expect(
        output.glyphs.empty() &&
            error.kind ==
                CatalogHarfBuzzShapingErrorKind::ResourceResolutionFailed &&
            error.resource_error.kind ==
                CatalogFontResourceErrorKind::InvalidFaceId &&
            !stats.resource_resolved && !stats.shaping_completed,
        "invalid face must preserve nested resolver error and clear output");

    request.face_id = 0U;
    request.cluster_limit =
        static_cast<std::uint32_t>(text.graphemes.size() + 5U);
    ok &= expect(
        !shape_catalog_harfbuzz_segment(request, &output, &stats, &error),
        "invalid shaping range must fail after resource resolution");
    ok &= expect(
        output.glyphs.empty() && stats.resource_resolved &&
            !stats.shaping_completed &&
            error.kind == CatalogHarfBuzzShapingErrorKind::ShapingFailed &&
            error.shaping_error.kind == HarfBuzzShapingErrorKind::InvalidInput,
        "shaping failure must remain nested and publish no glyphs");

    GenerationFixture missing;
    if (!missing.build(path.parent_path() / "missing-font.ttf", 712U)) {
        return false;
    }
    request = make_catalog_request(missing, text, &cache, font_size * 2U);
    ok &= expect(
        !shape_catalog_harfbuzz_segment(request, &output, &stats, &error),
        "missing catalog file must fail");
    ok &= expect(
        output.glyphs.empty() &&
            error.kind ==
                CatalogHarfBuzzShapingErrorKind::ResourceResolutionFailed &&
            error.resource_error.kind ==
                CatalogFontResourceErrorKind::FileLoadFailed &&
            error.resource_error.file_error.kind ==
                FontFileLoadErrorKind::MetadataFailed,
        "missing file must preserve complete resolver/file error chain");
    return ok && ledger.accounting_clean();
}

bool glyph_budget_failure_is_atomic(
    const std::filesystem::path& path,
    std::size_t font_size,
    const TextFixture& text) {
    GenerationFixture generation;
    if (!generation.build(path, 713U)) {
        return false;
    }
    VerifiedFontResourceCache cache(font_size * 3U, 64U * 1024U, 4U);
    const CatalogHarfBuzzShapingRequest request =
        make_catalog_request(generation, text, &cache, font_size * 2U);

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, 1U);
    LedgerMemoryResource memory(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun output(&memory);
    CatalogHarfBuzzShapingStats stats;
    CatalogHarfBuzzShapingError error;
    const bool result =
        shape_catalog_harfbuzz_segment(request, &output, &stats, &error);
    return expect(!result, "one-byte glyph budget must fail") &&
        expect(output.glyphs.empty(), "budget failure must publish no glyphs") &&
        expect(stats.resource_resolved && !stats.shaping_completed,
               "budget failure must preserve completed resource stage") &&
        expect(error.kind == CatalogHarfBuzzShapingErrorKind::ShapingFailed &&
                   error.shaping_error.kind ==
                       HarfBuzzShapingErrorKind::OutputBudgetExceeded,
               "budget failure must preserve nested HarfBuzz error") &&
        expect(ledger.accounting_clean(),
               "glyph ledger must remain clean after budget failure");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: catalog_harfbuzz_shaper_tests FONT\n";
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

    TextFixture text;
    if (!text.build()) {
        return 2;
    }
    const std::size_t font_size = static_cast<std::size_t>(file_size);
    bool ok = true;
    ok &= success_cache_and_equivalence(font_path, font_size, text);
    ok &= nested_failures_clear_output(font_path, font_size, text);
    ok &= glyph_budget_failure_is_atomic(font_path, font_size, text);
    if (!ok) {
        return 1;
    }
    std::cout << "catalog-backed HarfBuzz shaping tests passed\n";
    return 0;
}
