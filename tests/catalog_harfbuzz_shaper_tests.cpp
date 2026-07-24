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
    append_field(&result, "BoundCatalogPS");
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
    std::string family{"Bound Catalog Family"};
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

BoundCatalogHarfBuzzShapingRequest make_bound_request(
    const CatalogFontFaceBinding* binding,
    const TextFixture& text) {
    BoundCatalogHarfBuzzShapingRequest request;
    request.binding = binding;
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
    const CatalogFontFaceBinding& binding,
    const TextFixture& text) {
    HarfBuzzShapingRequest request;
    request.face_index = binding.resource()->view().face_index();
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
    request.verified_font_resource = binding.resource();
    return request;
}

bool same_resource_snapshot(
    const ResourceSnapshot& left,
    const ResourceSnapshot& right) {
    return left.hard_limit_bytes == right.hard_limit_bytes &&
        left.current_bytes == right.current_bytes &&
        left.peak_bytes == right.peak_bytes &&
        left.reservations == right.reservations &&
        left.releases == right.releases &&
        left.rejected_reservations == right.rejected_reservations &&
        left.accounting_errors == right.accounting_errors &&
        left.cache_hits == right.cache_hits &&
        left.cache_misses == right.cache_misses &&
        left.evictions == right.evictions &&
        left.physical_read_bytes == right.physical_read_bytes &&
        left.physical_write_bytes == right.physical_write_bytes;
}

bool same_cache_snapshot(
    const VerifiedFontResourceCacheStats& left,
    const VerifiedFontResourceCacheStats& right) {
    return same_resource_snapshot(left.metadata, right.metadata) &&
        same_resource_snapshot(left.retention, right.retention) &&
        left.hits == right.hits && left.misses == right.misses &&
        left.waits == right.waits &&
        left.build_attempts == right.build_attempts &&
        left.builds_published == right.builds_published &&
        left.build_failures == right.build_failures &&
        left.key_collisions == right.key_collisions &&
        left.evictions == right.evictions && left.clears == right.clears &&
        left.next_resource_id == right.next_resource_id &&
        left.entry_count == right.entry_count &&
        left.inflight_count == right.inflight_count &&
        left.maximum_entries == right.maximum_entries;
}

bool bind_real_face(
    GenerationFixture* generation,
    const std::filesystem::path& path,
    std::uint64_t generation_id,
    std::size_t font_size,
    VerifiedFontResourceCache* cache,
    CatalogFontFaceBinding* binding,
    CatalogFontResourceStats* stats) {
    if (!generation->build(path, generation_id)) {
        return false;
    }
    CatalogFontResourceError error;
    return bind_catalog_font_face(
        generation->generation,
        0U,
        font_size * 2U,
        cache,
        binding,
        stats,
        &error);
}

bool hot_path_is_io_free_and_equivalent(
    const std::filesystem::path& path,
    std::size_t font_size,
    const TextFixture& text) {
    GenerationFixture generation;
    VerifiedFontResourceCache cache(font_size * 3U, 64U * 1024U, 4U);
    CatalogFontFaceBinding binding;
    CatalogFontResourceStats binding_stats;
    bool ok = expect(
        bind_real_face(
            &generation,
            path,
            810U,
            font_size,
            &cache,
            &binding,
            &binding_stats),
        "catalog face must bind once");
    ok &= expect(
        binding.valid() && binding.generation_id() == 810U &&
            binding.face_id() == 0U && binding.resource_id() != 0U &&
            binding_stats.file_load.cache.build_attempts == 1U &&
            binding_stats.file_load.cache.builds_published == 1U,
        "binding must retain exact generation, face, resource, and cold-load evidence");
    if (!binding.valid()) {
        return false;
    }

    generation.generation.reset();
    cache.clear();
    const VerifiedFontResourceCacheStats cache_before = cache.snapshot();

    ResourceLedger first_ledger;
    first_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource first_memory(first_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun first_output(&first_memory);
    BoundCatalogHarfBuzzShapingStats first_stats;
    BoundCatalogHarfBuzzShapingError first_error;
    const BoundCatalogHarfBuzzShapingRequest request =
        make_bound_request(&binding, text);
    ok &= expect(
        shape_bound_catalog_harfbuzz_segment(
            request,
            &first_output,
            &first_stats,
            &first_error),
        "bound catalog face must shape after external generation reset and cache clear");
    const VerifiedFontResourceCacheStats cache_after_first = cache.snapshot();
    ok &= expect(
        same_cache_snapshot(cache_before, cache_after_first),
        "hot bound shaping must not touch the resolver cache");
    ok &= expect(
        first_stats.shaping_completed && first_stats.generation_id == 810U &&
            first_stats.face_id == 0U &&
            first_stats.resource_id == binding.resource_id() &&
            first_stats.shaping.used_verified_font_resource &&
            !first_stats.shaping.performed_inline_font_verification &&
            !first_output.glyphs.empty(),
        "bound shaping must publish retained-resource evidence");

    ResourceLedger second_ledger;
    second_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource second_memory(second_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun second_output(&second_memory);
    BoundCatalogHarfBuzzShapingStats second_stats;
    BoundCatalogHarfBuzzShapingError second_error;
    ok &= expect(
        shape_bound_catalog_harfbuzz_segment(
            request,
            &second_output,
            &second_stats,
            &second_error),
        "second hot bound shape must succeed");
    const VerifiedFontResourceCacheStats cache_after_second = cache.snapshot();
    ok &= expect(
        same_cache_snapshot(cache_before, cache_after_second),
        "repeated hot bound shaping must remain cache and I/O free");
    ok &= expect(
        std::vector<ShapedGlyph>(
            first_output.glyphs.begin(), first_output.glyphs.end()) ==
            std::vector<ShapedGlyph>(
                second_output.glyphs.begin(), second_output.glyphs.end()),
        "repeated bound shaping must be byte-exact");

    ResourceLedger direct_ledger;
    direct_ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource direct_memory(direct_ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun direct_output(&direct_memory);
    HarfBuzzShapingStats direct_stats;
    HarfBuzzShapingError direct_error;
    ok &= expect(
        shape_harfbuzz_segment(
            make_direct_request(binding, text),
            &direct_output,
            &direct_stats,
            &direct_error),
        "direct retained-resource path must shape");
    ok &= expect(
        std::vector<ShapedGlyph>(
            first_output.glyphs.begin(), first_output.glyphs.end()) ==
            std::vector<ShapedGlyph>(
                direct_output.glyphs.begin(), direct_output.glyphs.end()) &&
            direct_stats.verified_font_resource_id == binding.resource_id(),
        "bound and direct retained paths must be byte-exact equivalents");
    return ok && first_ledger.accounting_clean() &&
        second_ledger.accounting_clean() && direct_ledger.accounting_clean();
}

bool binding_failures_are_atomic(
    const std::filesystem::path& path,
    std::size_t font_size) {
    GenerationFixture generation;
    VerifiedFontResourceCache cache(font_size * 3U, 64U * 1024U, 4U);
    CatalogFontFaceBinding binding;
    CatalogFontResourceStats stats;
    CatalogFontResourceError error;
    if (!generation.build(path, 811U) ||
        !bind_catalog_font_face(
            generation.generation,
            0U,
            font_size * 2U,
            &cache,
            &binding,
            &stats,
            &error)) {
        return false;
    }

    bool ok = expect(
        !bind_catalog_font_face(
            generation.generation,
            kInvalidFontFaceId,
            font_size * 2U,
            &cache,
            &binding,
            &stats,
            &error),
        "invalid face rebinding must fail");
    ok &= expect(
        !binding.valid() &&
            error.kind == CatalogFontResourceErrorKind::InvalidFaceId,
        "invalid face rebinding must clear the previous binding");

    GenerationFixture missing;
    if (!missing.build(path.parent_path() / "missing-font.ttf", 812U)) {
        return false;
    }
    ok &= expect(
        !bind_catalog_font_face(
            missing.generation,
            0U,
            font_size * 2U,
            &cache,
            &binding,
            &stats,
            &error),
        "missing catalog font must fail binding");
    ok &= expect(
        !binding.valid() &&
            error.kind == CatalogFontResourceErrorKind::FileLoadFailed &&
            error.file_error.kind == FontFileLoadErrorKind::MetadataFailed,
        "missing file must preserve nested resolver/file error and no binding");
    return ok;
}

bool hot_failures_are_atomic(
    const std::filesystem::path& path,
    std::size_t font_size,
    const TextFixture& text) {
    GenerationFixture generation;
    VerifiedFontResourceCache cache(font_size * 3U, 64U * 1024U, 4U);
    CatalogFontFaceBinding binding;
    CatalogFontResourceStats binding_stats;
    if (!bind_real_face(
            &generation,
            path,
            813U,
            font_size,
            &cache,
            &binding,
            &binding_stats)) {
        return false;
    }

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource memory(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun output(&memory);
    BoundCatalogHarfBuzzShapingStats stats;
    BoundCatalogHarfBuzzShapingError error;
    BoundCatalogHarfBuzzShapingRequest request =
        make_bound_request(&binding, text);
    bool ok = expect(
        shape_bound_catalog_harfbuzz_segment(
            request,
            &output,
            &stats,
            &error),
        "hot failure fixture must first seed glyph output");

    CatalogFontFaceBinding invalid_binding;
    request.binding = &invalid_binding;
    ok &= expect(
        !shape_bound_catalog_harfbuzz_segment(
            request,
            &output,
            &stats,
            &error),
        "default binding must fail");
    ok &= expect(
        output.glyphs.empty() &&
            error.kind == BoundCatalogHarfBuzzShapingErrorKind::InvalidBinding &&
            !stats.shaping_completed,
        "invalid binding must clear prior glyph output");

    request = make_bound_request(&binding, text);
    request.cluster_limit =
        static_cast<std::uint32_t>(text.graphemes.size() + 5U);
    ok &= expect(
        !shape_bound_catalog_harfbuzz_segment(
            request,
            &output,
            &stats,
            &error),
        "invalid cluster range must fail");
    ok &= expect(
        output.glyphs.empty() &&
            error.kind == BoundCatalogHarfBuzzShapingErrorKind::ShapingFailed &&
            error.shaping_error.kind == HarfBuzzShapingErrorKind::InvalidInput &&
            !stats.shaping_completed,
        "invalid cluster range must preserve nested shaping error");
    return ok && ledger.accounting_clean();
}

bool glyph_budget_failure_is_atomic(
    const std::filesystem::path& path,
    std::size_t font_size,
    const TextFixture& text) {
    GenerationFixture generation;
    VerifiedFontResourceCache cache(font_size * 3U, 64U * 1024U, 4U);
    CatalogFontFaceBinding binding;
    CatalogFontResourceStats binding_stats;
    if (!bind_real_face(
            &generation,
            path,
            814U,
            font_size,
            &cache,
            &binding,
            &binding_stats)) {
        return false;
    }

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphRun, 1U);
    LedgerMemoryResource memory(ledger, ResourceClass::GlyphRun);
    ShapedGlyphRun output(&memory);
    BoundCatalogHarfBuzzShapingStats stats;
    BoundCatalogHarfBuzzShapingError error;
    const bool result = shape_bound_catalog_harfbuzz_segment(
        make_bound_request(&binding, text),
        &output,
        &stats,
        &error);
    return expect(!result, "one-byte glyph budget must fail") &&
        expect(output.glyphs.empty(), "budget failure must publish no glyphs") &&
        expect(!stats.shaping_completed &&
                   error.kind ==
                       BoundCatalogHarfBuzzShapingErrorKind::ShapingFailed &&
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
    ok &= hot_path_is_io_free_and_equivalent(font_path, font_size, text);
    ok &= binding_failures_are_atomic(font_path, font_size);
    ok &= hot_failures_are_atomic(font_path, font_size, text);
    ok &= glyph_budget_failure_is_atomic(font_path, font_size, text);
    if (!ok) {
        return 1;
    }
    std::cout << "bound catalog HarfBuzz shaping tests passed\n";
    return 0;
}
