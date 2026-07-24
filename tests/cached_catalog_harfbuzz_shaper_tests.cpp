#include "cached_catalog_harfbuzz_shaper.hpp"
#include "grapheme_segmenter.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_script.hpp"
#include "unicode_stream.hpp"

#include <array>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
using namespace zevryon::core;
using namespace zevryon::text;
constexpr std::size_t kGlyphLimit = 1024U * 1024U;

bool expect(bool value, const char* message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
void field(std::string* out, std::string_view value) {
    out->append(std::to_string(value.size())); out->push_back(':');
    out->append(value); out->push_back('|');
}
std::string utf8_path(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
ScriptId latin() {
    ScriptId value = ScriptId::Zzzz;
    (void)script_id_from_name("Latn", &value);
    return value;
}
std::vector<ShapedGlyph> copy(const ShapedGlyphRun& run) {
    return {run.glyphs.begin(), run.glyphs.end()};
}

struct Fixture {
    std::pmr::monotonic_buffer_resource memory;
    std::pmr::vector<DecodedCodePoint> codepoints{&memory};
    std::pmr::vector<GraphemeBoundary> graphemes{&memory};
    std::string identity;
    std::string family{"Cached Catalog Family"};
    std::array<FontCoverageRange, 1> coverage{{{0x20U, 0x7eU}}};
    std::array<FontDiscoveryFace, 1> faces;
    std::shared_ptr<const FontCatalogGeneration> generation;
    CatalogFontFaceBinding binding;

    bool build(const std::filesystem::path& path, std::size_t size,
               VerifiedFontResourceCache* resources) {
        constexpr std::string_view text = "office affine efficient a\xCC\x81";
        Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict);
        Utf8DecodeError decode_error;
        const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
        if (!decoder.feed(bytes, 0U, &codepoints, &decode_error) ||
            !decoder.finish(&codepoints, &decode_error)) return false;
        GraphemeSegmentStats gs; GraphemeError ge;
        if (!segment_graphemes(codepoints, &graphemes, &gs, &ge)) return false;
        identity = "fontconfig|";
        field(&identity, ""); field(&identity, utf8_path(path));
        field(&identity, "0"); field(&identity, "CachedCatalogPS");
        field(&identity, "");
        faces[0] = FontDiscoveryFace{identity, family, 400U, 5U,
            FontSlant::Upright, latin(), 0U, coverage};
        FontDiscoveryStats ds; FontDiscoveryError de;
        if (!build_font_catalog_generation(1310U, faces, 2U * 1024U * 1024U,
                256U * 1024U, &generation, &ds, &de)) return false;
        CatalogFontResourceStats stats; CatalogFontResourceError error;
        return bind_catalog_font_face(generation, 0U, size * 2U, resources,
            &binding, &stats, &error);
    }

    CachedCatalogHarfBuzzShapingRequest cached(
        PreparedHarfBuzzFaceCache* cache) const {
        CachedCatalogHarfBuzzShapingRequest r;
        r.binding = &binding; r.prepared_face_cache = cache;
        r.codepoints = codepoints; r.grapheme_boundaries = graphemes;
        r.cluster_limit = static_cast<std::uint32_t>(graphemes.size() - 1U);
        r.script = latin(); r.language = "en";
        r.beginning_of_text = true; r.end_of_text = true;
        return r;
    }
    BoundCatalogHarfBuzzShapingRequest direct(
        std::shared_ptr<const PreparedHarfBuzzFace> face) const {
        BoundCatalogHarfBuzzShapingRequest r;
        r.codepoints = codepoints; r.grapheme_boundaries = graphemes;
        r.cluster_limit = static_cast<std::uint32_t>(graphemes.size() - 1U);
        r.script = latin(); r.language = "en";
        r.beginning_of_text = true; r.end_of_text = true;
        r.prepared_harfbuzz_face = std::move(face);
        return r;
    }
};

bool sequential(Fixture* f, VerifiedFontResourceCache* resources,
                std::size_t size) {
    f->generation.reset(); resources->clear();
    const auto resource_before = resources->snapshot();
    PreparedHarfBuzzFaceCache cache(size * 3U, 128U * 1024U, 4U);
    ResourceLedger a; a.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource am(a, ResourceClass::GlyphRun); ShapedGlyphRun ao(&am);
    CachedCatalogHarfBuzzShapingStats as; CachedCatalogHarfBuzzShapingError ae;
    bool ok = expect(shape_cached_catalog_harfbuzz_segment(
        f->cached(&cache), &ao, &as, &ae), "cold cached shape");
    ok &= expect(as.face_acquired && as.shaping_completed &&
        as.face_cache.preparation_attempts == 1U &&
        as.face_cache.faces_published == 1U &&
        as.shaping.shaping.used_prepared_harfbuzz_face,
        "cold cache and shaping evidence");

    ResourceLedger b; b.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource bm(b, ResourceClass::GlyphRun); ShapedGlyphRun bo(&bm);
    CachedCatalogHarfBuzzShapingStats bs; CachedCatalogHarfBuzzShapingError be;
    ok &= expect(shape_cached_catalog_harfbuzz_segment(
        f->cached(&cache), &bo, &bs, &be), "resident cached shape");
    ok &= expect(bs.face_cache.hits == 1U &&
        bs.face_cache.preparation_attempts == 1U && copy(ao) == copy(bo),
        "resident hit byte equivalence");

    std::shared_ptr<const PreparedHarfBuzzFace> face;
    PreparedHarfBuzzFaceCacheStats cs; PreparedHarfBuzzFaceCacheError ce;
    ok &= expect(cache.lookup(f->binding, &face, &cs, &ce), "resident lookup");
    ResourceLedger d; d.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource dm(d, ResourceClass::GlyphRun); ShapedGlyphRun direct(&dm);
    BoundCatalogHarfBuzzShapingStats ds; BoundCatalogHarfBuzzShapingError de;
    ok &= expect(shape_bound_catalog_harfbuzz_segment(
        f->direct(face), &direct, &ds, &de) && copy(bo) == copy(direct),
        "automatic and direct prepared equivalence");
    const auto resource_after = resources->snapshot();
    ok &= expect(resource_before.hits == resource_after.hits &&
        resource_before.misses == resource_after.misses &&
        resource_before.build_attempts == resource_after.build_attempts &&
        resource_before.entry_count == resource_after.entry_count,
        "verified resource cache remains untouched");
    return ok && a.accounting_clean() && b.accounting_clean() &&
        d.accounting_clean();
}

bool concurrent(const Fixture& f, std::size_t size) {
    PreparedHarfBuzzFaceCache cache(size * 3U, 128U * 1024U, 4U);
    constexpr std::size_t n = 12U;
    std::array<std::vector<ShapedGlyph>, n> glyphs;
    std::array<bool, n> result{};
    std::atomic<std::size_t> ready{0U}; std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < n; ++i) threads.emplace_back([&, i] {
        ready.fetch_add(1U);
        while (!go.load()) {
            std::this_thread::yield();
        }
        ResourceLedger l; l.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
        LedgerMemoryResource m(l, ResourceClass::GlyphRun); ShapedGlyphRun o(&m);
        CachedCatalogHarfBuzzShapingStats s; CachedCatalogHarfBuzzShapingError e;
        result[i] = shape_cached_catalog_harfbuzz_segment(
            f.cached(&cache), &o, &s, &e) && l.accounting_clean();
        glyphs[i] = copy(o);
    });
    while (ready.load() != n) {
        std::this_thread::yield();
    }
    go.store(true);
    for (auto& thread : threads) thread.join();
    bool ok = true;
    for (std::size_t i = 0; i < n; ++i)
        ok &= expect(result[i] && glyphs[i] == glyphs[0],
                     "concurrent cached glyph equivalence");
    const auto stats = cache.snapshot();
    return ok && expect(stats.preparation_attempts == 1U &&
        stats.faces_published == 1U && stats.entry_count == 1U,
        "concurrent shapes share one preparation");
}

bool failures(Fixture* f, std::size_t size) {
    PreparedHarfBuzzFaceCache cache(size * 2U, 64U * 1024U, 2U);
    ResourceLedger l; l.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    LedgerMemoryResource m(l, ResourceClass::GlyphRun); ShapedGlyphRun o(&m);
    CachedCatalogHarfBuzzShapingStats s; CachedCatalogHarfBuzzShapingError e;
    if (!shape_cached_catalog_harfbuzz_segment(f->cached(&cache), &o, &s, &e))
        return false;
    CatalogFontFaceBinding invalid;
    auto r = f->cached(&cache); r.binding = &invalid;
    bool ok = expect(!shape_cached_catalog_harfbuzz_segment(r, &o, &s, &e) &&
        o.glyphs.empty() && e.kind ==
        CachedCatalogHarfBuzzShapingErrorKind::InvalidArgument,
        "invalid binding atomic failure");
    PreparedHarfBuzzFaceCache small(size - 1U, 64U * 1024U, 2U);
    ok &= expect(!shape_cached_catalog_harfbuzz_segment(
        f->cached(&small), &o, &s, &e) && !s.face_acquired &&
        e.kind == CachedCatalogHarfBuzzShapingErrorKind::FaceCacheFailed &&
        e.face_cache_error.kind ==
        PreparedHarfBuzzFaceCacheErrorKind::BindingExceedsRetentionLimit,
        "nested cache admission failure");
    r = f->cached(&cache);
    r.cluster_limit = static_cast<std::uint32_t>(f->graphemes.size() + 1U);
    ok &= expect(!shape_cached_catalog_harfbuzz_segment(r, &o, &s, &e) &&
        s.face_acquired && !s.shaping_completed &&
        e.kind == CachedCatalogHarfBuzzShapingErrorKind::ShapingFailed &&
        e.shaping_error.shaping_error.kind == HarfBuzzShapingErrorKind::InvalidInput,
        "nested shaping failure");
    ResourceLedger tiny; tiny.set_hard_limit(ResourceClass::GlyphRun, 1U);
    LedgerMemoryResource tm(tiny, ResourceClass::GlyphRun); ShapedGlyphRun to(&tm);
    ok &= expect(!shape_cached_catalog_harfbuzz_segment(
        f->cached(&cache), &to, &s, &e) && to.glyphs.empty() &&
        e.shaping_error.shaping_error.kind ==
        HarfBuzzShapingErrorKind::OutputBudgetExceeded,
        "nested glyph budget failure");
    return ok && l.accounting_clean() && tiny.accounting_clean();
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path path(argv[1]); std::error_code ec;
    const auto raw = std::filesystem::file_size(path, ec);
    if (ec || raw == 0U || raw > std::numeric_limits<std::size_t>::max() / 6U)
        return 2;
    const auto size = static_cast<std::size_t>(raw);
    VerifiedFontResourceCache resources(size * 3U, 128U * 1024U, 4U);
    Fixture fixture;
    if (!fixture.build(path, size, &resources)) return 2;
    const bool ok = sequential(&fixture, &resources, size) &&
        concurrent(fixture, size) && failures(&fixture, size);
    if (!ok) return 1;
    std::cout << "cache-backed catalog HarfBuzz shaping tests passed\n";
    return 0;
}
