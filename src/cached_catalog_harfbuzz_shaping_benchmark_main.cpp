#include "cached_catalog_harfbuzz_shaper.hpp"
#include "grapheme_segmenter.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "unicode_script.hpp"
#include "unicode_stream.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {
using namespace zevryon::core;
using namespace zevryon::text;
constexpr std::size_t kWarm = 128U, kBatch = 128U, kSamples = 48U;
constexpr std::size_t kGlyphLimit = 1024U * 1024U;

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
    (void)script_id_from_name("Latn", &value); return value;
}
double pct(const std::vector<double>& v, double p) {
    const double x = p * static_cast<double>(v.size() - 1U);
    const auto lo = static_cast<std::size_t>(x);
    const auto hi = std::min(lo + 1U, v.size() - 1U);
    return v[lo] * (1.0 - (x - lo)) + v[hi] * (x - lo);
}

struct Fixture {
    std::string utf8;
    std::pmr::monotonic_buffer_resource memory;
    std::pmr::vector<DecodedCodePoint> codepoints{&memory};
    std::pmr::vector<GraphemeBoundary> graphemes{&memory};
    std::string identity, family{"Cached Benchmark Family"};
    std::array<FontCoverageRange, 1> coverage{{{0x20U, 0x7eU}}};
    std::array<FontDiscoveryFace, 1> faces;
    CatalogFontFaceBinding binding;

    bool build(const std::filesystem::path& path, std::size_t size) {
        constexpr std::string_view pattern = "office affine a\xCC\x81 ";
        while (utf8.size() + pattern.size() <= 96U) utf8.append(pattern);
        Utf8StreamDecoder decoder(Utf8ErrorPolicy::Strict); Utf8DecodeError de;
        const auto bytes = std::as_bytes(std::span(utf8.data(), utf8.size()));
        if (!decoder.feed(bytes, 0U, &codepoints, &de) ||
            !decoder.finish(&codepoints, &de)) return false;
        GraphemeSegmentStats gs; GraphemeError ge;
        if (!segment_graphemes(codepoints, &graphemes, &gs, &ge)) return false;
        identity = "fontconfig|"; field(&identity, "");
        field(&identity, utf8_path(path)); field(&identity, "0");
        field(&identity, "CachedBenchmarkPS"); field(&identity, "");
        faces[0] = FontDiscoveryFace{identity, family, 400U, 5U,
            FontSlant::Upright, latin(), 0U, coverage};
        std::shared_ptr<const FontCatalogGeneration> generation;
        FontDiscoveryStats ds; FontDiscoveryError derr;
        if (!build_font_catalog_generation(1320U, faces, 2U * 1024U * 1024U,
                256U * 1024U, &generation, &ds, &derr)) return false;
        VerifiedFontResourceCache resources(size * 3U, 128U * 1024U, 4U);
        CatalogFontResourceStats bs; CatalogFontResourceError be;
        if (!bind_catalog_font_face(generation, 0U, size * 2U, &resources,
                &binding, &bs, &be)) return false;
        generation.reset(); resources.clear(); return binding.valid();
    }
    BoundCatalogHarfBuzzShapingRequest plain() const {
        BoundCatalogHarfBuzzShapingRequest r; r.binding = &binding;
        r.codepoints = codepoints; r.grapheme_boundaries = graphemes;
        r.cluster_limit = static_cast<std::uint32_t>(graphemes.size() - 1U);
        r.script = latin(); r.language = "en";
        r.beginning_of_text = true; r.end_of_text = true; return r;
    }
    BoundCatalogHarfBuzzShapingRequest direct(
        std::shared_ptr<const PreparedHarfBuzzFace> face) const {
        auto r = plain(); r.binding = nullptr;
        r.prepared_harfbuzz_face = std::move(face); return r;
    }
    CachedCatalogHarfBuzzShapingRequest cached(
        PreparedHarfBuzzFaceCache* cache) const {
        CachedCatalogHarfBuzzShapingRequest r; r.binding = &binding;
        r.prepared_face_cache = cache; r.codepoints = codepoints;
        r.grapheme_boundaries = graphemes;
        r.cluster_limit = static_cast<std::uint32_t>(graphemes.size() - 1U);
        r.script = latin(); r.language = "en";
        r.beginning_of_text = true; r.end_of_text = true; return r;
    }
};

struct Measure {
    Measure() : memory(ledger, ResourceClass::GlyphRun), output(&memory) {
        ledger.set_hard_limit(ResourceClass::GlyphRun, kGlyphLimit);
    }
    ResourceLedger ledger; LedgerMemoryResource memory; ShapedGlyphRun output;
    HarfBuzzShapingStats shaping; std::uint64_t generation{0}, resource{0};
    FontFaceId face{kInvalidFontFaceId}; std::vector<double> samples;
};

bool call_bound(const BoundCatalogHarfBuzzShapingRequest& request, Measure* m) {
    BoundCatalogHarfBuzzShapingStats stats; BoundCatalogHarfBuzzShapingError err;
    if (!shape_bound_catalog_harfbuzz_segment(
            request, &m->output, &stats, &err)) return false;
    m->shaping = stats.shaping; m->generation = stats.generation_id;
    m->face = stats.face_id; m->resource = stats.resource_id; return true;
}
bool call_cached(const CachedCatalogHarfBuzzShapingRequest& request, Measure* m) {
    CachedCatalogHarfBuzzShapingStats stats; CachedCatalogHarfBuzzShapingError err;
    if (!shape_cached_catalog_harfbuzz_segment(
            request, &m->output, &stats, &err)) return false;
    m->shaping = stats.shaping.shaping;
    m->generation = stats.shaping.generation_id;
    m->face = stats.shaping.face_id; m->resource = stats.shaping.resource_id;
    return true;
}

template<class Fn> bool calls(Fn&& fn, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) if (!fn()) return false;
    return true;
}
template<class Fn> double batch(Fn&& fn, bool* ok) {
    const auto start = std::chrono::steady_clock::now();
    *ok = calls(fn, kBatch);
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count() /
        static_cast<double>(kBatch);
}
std::vector<ShapedGlyph> glyphs(const Measure& m) {
    return {m.output.glyphs.begin(), m.output.glyphs.end()};
}
bool valid(const Measure& m, bool prepared) {
    const auto snap = m.ledger.snapshot(ResourceClass::GlyphRun);
    const auto bytes = m.output.glyphs.size() * sizeof(ShapedGlyph);
    return !m.output.glyphs.empty() && m.shaping.missing_glyphs == 0U &&
        m.shaping.used_verified_font_resource &&
        m.shaping.used_prepared_harfbuzz_face == prepared &&
        !m.shaping.performed_inline_font_verification &&
        snap.current_bytes == bytes && m.ledger.accounting_clean() &&
        m.ledger.within_hard_limits();
}
void emit(std::string_view mode, const Fixture& f, Measure* m) {
    std::sort(m->samples.begin(), m->samples.end());
    const auto snap = m->ledger.snapshot(ResourceClass::GlyphRun);
    std::cout << std::fixed << std::setprecision(9)
      << "{\"schema\":\"zevryon.cached-catalog-shaping-benchmark.v1\","
      << "\"mode\":\"" << mode << "\","
      << "\"input_utf8_bytes\":" << f.utf8.size() << ','
      << "\"input_codepoints\":" << f.codepoints.size() << ','
      << "\"input_clusters\":" << f.graphemes.size() - 1U << ','
      << "\"output_glyphs\":" << m->output.glyphs.size() << ','
      << "\"output_bytes\":" << snap.current_bytes << ','
      << "\"total_x_advance\":" << m->shaping.total_x_advance << ','
      << "\"units_per_em\":" << m->shaping.units_per_em << ','
      << "\"generation_id\":" << m->generation << ','
      << "\"face_id\":" << m->face << ','
      << "\"resource_id\":" << m->resource << ','
      << "\"prepared\":"
      << (m->shaping.used_prepared_harfbuzz_face ? "true" : "false") << ','
      << "\"p50_ms\":" << pct(m->samples, .50) << ','
      << "\"p95_ms\":" << pct(m->samples, .95) << ','
      << "\"p99_ms\":" << pct(m->samples, .99) << ','
      << "\"maximum_ms\":" << m->samples.back() << ','
      << "\"accounting_clean\":true,\"within_hard_limits\":true}\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path path(argv[1]); std::error_code ec;
    const auto raw = std::filesystem::file_size(path, ec);
    if (ec || raw == 0U || raw > std::numeric_limits<std::size_t>::max() / 6U)
        return 2;
    const auto size = static_cast<std::size_t>(raw);
    Fixture f; if (!f.build(path, size)) return 2;
    PreparedHarfBuzzFaceCache cache(size * 3U, 128U * 1024U, 4U);
    std::shared_ptr<const PreparedHarfBuzzFace> prepared;
    PreparedHarfBuzzFaceCacheStats cs; PreparedHarfBuzzFaceCacheError ce;
    if (!cache.get_or_prepare(f.binding, &prepared, &cs, &ce)) return 2;
    const auto plain_request = f.plain();
    const auto direct_request = f.direct(prepared);
    const auto cached_request = f.cached(&cache);
    Measure plain, direct, cached;
    auto p = [&] { return call_bound(plain_request, &plain); };
    auto d = [&] { return call_bound(direct_request, &direct); };
    auto c = [&] { return call_cached(cached_request, &cached); };
    if (!calls(p, kWarm) || !calls(d, kWarm) || !calls(c, kWarm)) return 1;
    plain.samples.reserve(kSamples); direct.samples.reserve(kSamples);
    cached.samples.reserve(kSamples);
    for (std::size_t i = 0; i < kSamples; ++i) {
        bool a=false,b=false,e=false;
        if (i % 3U == 0U) {
            plain.samples.push_back(batch(p,&a)); direct.samples.push_back(batch(d,&b));
            cached.samples.push_back(batch(c,&e));
        } else if (i % 3U == 1U) {
            direct.samples.push_back(batch(d,&a)); cached.samples.push_back(batch(c,&b));
            plain.samples.push_back(batch(p,&e));
        } else {
            cached.samples.push_back(batch(c,&a)); plain.samples.push_back(batch(p,&b));
            direct.samples.push_back(batch(d,&e));
        }
        if (!a || !b || !e) return 1;
    }
    if (!valid(plain,false) || !valid(direct,true) || !valid(cached,true) ||
        glyphs(plain) != glyphs(direct) || glyphs(direct) != glyphs(cached) ||
        plain.shaping.total_x_advance != direct.shaping.total_x_advance ||
        direct.shaping.total_x_advance != cached.shaping.total_x_advance ||
        plain.generation != direct.generation || direct.generation != cached.generation ||
        plain.resource != direct.resource || direct.resource != cached.resource)
        return 1;
    emit("plain_binding_call", f, &plain);
    emit("direct_prepared_call", f, &direct);
    emit("cached_prepared_call", f, &cached);
    return 0;
}
