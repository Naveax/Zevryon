#include "prepared_harfbuzz_face_cache.hpp"

#include "unicode_script.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using namespace zevryon::text;

constexpr std::size_t kDiscoveryLimit = 2U * 1024U * 1024U;
constexpr std::size_t kCatalogLimit = 256U * 1024U;

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

struct BindingFixture {
    std::string identity;
    std::string family;
    std::array<FontCoverageRange, 1> coverage{{{0x0020U, 0x007eU}}};
    std::array<FontDiscoveryFace, 1> faces;
    std::shared_ptr<const FontCatalogGeneration> generation;
    CatalogFontFaceBinding binding;
    CatalogFontResourceStats stats;

    bool build(
        const std::filesystem::path& path,
        std::size_t font_size,
        std::uint64_t generation_id,
        std::string_view suffix,
        VerifiedFontResourceCache* resource_cache) {
        family = "Prepared Cache Family ";
        family.append(suffix);
        identity = "fontconfig|";
        append_field(&identity, "");
        append_field(&identity, path_utf8(path));
        append_field(&identity, "0");
        std::string postscript = "PreparedCachePS";
        postscript.append(suffix);
        append_field(&identity, postscript);
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
                generation_id,
                faces,
                kDiscoveryLimit,
                kCatalogLimit,
                &generation,
                &discovery_stats,
                &discovery_error)) {
            return false;
        }
        CatalogFontResourceError error;
        return bind_catalog_font_face(
            generation,
            0U,
            font_size * 2U,
            resource_cache,
            &binding,
            &stats,
            &error);
    }
};

bool cold_hit_clear_lifetime(
    const std::filesystem::path& path,
    std::size_t font_size) {
    VerifiedFontResourceCache resource_cache(
        font_size * 3U,
        64U * 1024U,
        4U);
    BindingFixture fixture;
    if (!fixture.build(
            path,
            font_size,
            1210U,
            "Cold",
            &resource_cache)) {
        return false;
    }
    bool ok = expect(
        fixture.binding.valid() &&
            fixture.binding.content_identity() ==
                fixture.stats.file_load.identity &&
            fixture.binding.content_identity().face_index == 0U,
        "binding must retain the resolver content identity");

    fixture.generation.reset();
    resource_cache.clear();
    PreparedHarfBuzzFaceCache cache(
        font_size * 3U,
        64U * 1024U,
        4U);
    std::shared_ptr<const PreparedHarfBuzzFace> first;
    PreparedHarfBuzzFaceCacheStats stats;
    PreparedHarfBuzzFaceCacheError error;
    ok &= expect(
        cache.get_or_prepare(fixture.binding, &first, &stats, &error),
        "cold prepared-face cache miss must build");
    ok &= expect(
        first && first->valid() && stats.misses == 1U &&
            stats.preparation_attempts == 1U &&
            stats.faces_published == 1U && stats.entry_count == 1U &&
            stats.retention.current_bytes == font_size,
        "cold miss must publish exact preparation and retention evidence");

    std::shared_ptr<const PreparedHarfBuzzFace> second;
    ok &= expect(
        cache.get_or_prepare(fixture.binding, &second, &stats, &error) &&
            second == first && stats.hits == 1U &&
            stats.preparation_attempts == 1U,
        "repeated get must return the identical resident prepared face");

    std::shared_ptr<const PreparedHarfBuzzFace> lookup;
    ok &= expect(
        cache.lookup(fixture.binding, &lookup, &stats, &error) &&
            lookup == first && stats.hits == 2U,
        "lookup must return the resident prepared face without preparation");

    const std::uint64_t resource_id = first->resource_id();
    cache.clear();
    const PreparedHarfBuzzFaceCacheStats cleared = cache.snapshot();
    ok &= expect(
        cleared.entry_count == 0U && cleared.retention.current_bytes == 0U &&
            cleared.clears == 1U && first->valid() &&
            first->resource_id() == resource_id,
        "cache clear must release cache retention without invalidating caller handles");

    std::shared_ptr<const PreparedHarfBuzzFace> rebuilt;
    ok &= expect(
        cache.get_or_prepare(fixture.binding, &rebuilt, &stats, &error) &&
            rebuilt && rebuilt != first && stats.preparation_attempts == 2U &&
            stats.faces_published == 2U,
        "post-clear get must prepare a new resident native face");
    return ok;
}

bool concurrent_single_flight(
    const std::filesystem::path& path,
    std::size_t font_size) {
    VerifiedFontResourceCache resource_cache(
        font_size * 3U,
        64U * 1024U,
        4U);
    BindingFixture fixture;
    if (!fixture.build(
            path,
            font_size,
            1220U,
            "Concurrent",
            &resource_cache)) {
        return false;
    }
    fixture.generation.reset();
    resource_cache.clear();

    PreparedHarfBuzzFaceCache cache(
        font_size * 3U,
        128U * 1024U,
        4U);
    constexpr std::size_t kThreads = 16U;
    std::array<std::shared_ptr<const PreparedHarfBuzzFace>, kThreads> outputs;
    std::array<bool, kThreads> results{};
    std::atomic<std::size_t> ready{0U};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t index = 0U; index < kThreads; ++index) {
        threads.emplace_back([&, index] {
            ready.fetch_add(1U, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            PreparedHarfBuzzFaceCacheStats stats;
            PreparedHarfBuzzFaceCacheError error;
            results[index] = cache.get_or_prepare(
                fixture.binding,
                &outputs[index],
                &stats,
                &error);
        });
    }
    while (ready.load(std::memory_order_acquire) != kThreads) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }

    bool ok = true;
    for (std::size_t index = 0U; index < kThreads; ++index) {
        ok &= expect(
            results[index] && outputs[index] &&
                outputs[index] == outputs[0],
            "all concurrent callers must receive one shared prepared face");
    }
    const PreparedHarfBuzzFaceCacheStats stats = cache.snapshot();
    ok &= expect(
        stats.preparation_attempts == 1U && stats.faces_published == 1U &&
            stats.preparation_failures == 0U && stats.entry_count == 1U &&
            stats.inflight_count == 0U,
        "concurrent misses must publish exactly one preparation");
    return ok;
}

bool deterministic_lru(
    const std::filesystem::path& path,
    std::size_t font_size) {
    VerifiedFontResourceCache resource_cache(
        font_size * 3U,
        128U * 1024U,
        4U);
    BindingFixture first;
    BindingFixture second;
    BindingFixture third;
    if (!first.build(path, font_size, 1230U, "One", &resource_cache) ||
        !second.build(path, font_size, 1231U, "Two", &resource_cache) ||
        !third.build(path, font_size, 1232U, "Three", &resource_cache)) {
        return false;
    }
    first.generation.reset();
    second.generation.reset();
    third.generation.reset();
    resource_cache.clear();

    PreparedHarfBuzzFaceCache cache(
        font_size * 3U,
        128U * 1024U,
        2U);
    PreparedHarfBuzzFaceCacheStats stats;
    PreparedHarfBuzzFaceCacheError error;
    std::shared_ptr<const PreparedHarfBuzzFace> output;
    bool ok = expect(
        cache.get_or_prepare(first.binding, &output, &stats, &error) &&
            cache.get_or_prepare(second.binding, &output, &stats, &error) &&
            cache.get_or_prepare(first.binding, &output, &stats, &error) &&
            cache.get_or_prepare(third.binding, &output, &stats, &error),
        "three generation-local prepared faces must populate the bounded LRU");

    std::shared_ptr<const PreparedHarfBuzzFace> resident;
    ok &= expect(
        cache.lookup(first.binding, &resident, &stats, &error),
        "recently touched first face must remain resident");
    ok &= expect(
        !cache.lookup(second.binding, &resident, &stats, &error) &&
            !resident &&
            error.kind == PreparedHarfBuzzFaceCacheErrorKind::CacheMiss,
        "least-recently-used second face must be evicted");
    ok &= expect(
        cache.lookup(third.binding, &resident, &stats, &error),
        "new third face must remain resident");
    const PreparedHarfBuzzFaceCacheStats snapshot = cache.snapshot();
    ok &= expect(
        snapshot.evictions == 1U && snapshot.entry_count == 2U &&
            snapshot.preparation_attempts == 3U &&
            snapshot.faces_published == 3U &&
            snapshot.retention.current_bytes == font_size * 2U,
        "LRU eviction and conservative retention accounting must be exact");
    return ok;
}

bool limits_and_failures_are_atomic(
    const std::filesystem::path& path,
    std::size_t font_size) {
    VerifiedFontResourceCache resource_cache(
        font_size * 3U,
        64U * 1024U,
        4U);
    BindingFixture fixture;
    if (!fixture.build(
            path,
            font_size,
            1240U,
            "Limits",
            &resource_cache)) {
        return false;
    }

    PreparedHarfBuzzFaceCache cache(
        font_size * 2U,
        64U * 1024U,
        2U);
    std::shared_ptr<const PreparedHarfBuzzFace> output;
    PreparedHarfBuzzFaceCacheStats stats;
    PreparedHarfBuzzFaceCacheError error;
    if (!cache.get_or_prepare(fixture.binding, &output, &stats, &error)) {
        return false;
    }

    CatalogFontFaceBinding invalid;
    bool ok = expect(
        !cache.get_or_prepare(invalid, &output, &stats, &error) &&
            !output &&
            error.kind == PreparedHarfBuzzFaceCacheErrorKind::InvalidArgument,
        "invalid binding must fail and clear a previous output");

    PreparedHarfBuzzFaceCache too_small(
        font_size - 1U,
        64U * 1024U,
        2U);
    ok &= expect(
        !too_small.get_or_prepare(
            fixture.binding,
            &output,
            &stats,
            &error) &&
            !output &&
            error.kind ==
                PreparedHarfBuzzFaceCacheErrorKind::BindingExceedsRetentionLimit &&
            stats.preparation_attempts == 0U,
        "oversized binding must be rejected before native preparation");

    PreparedHarfBuzzFaceCache metadata_starved(
        font_size * 2U,
        1U,
        2U);
    ok &= expect(
        !metadata_starved.get_or_prepare(
            fixture.binding,
            &output,
            &stats,
            &error) &&
            !output &&
            error.kind ==
                PreparedHarfBuzzFaceCacheErrorKind::MetadataBudgetExceeded &&
            stats.metadata.rejected_reservations > 0U &&
            stats.preparation_attempts == 0U &&
            stats.entry_count == 0U,
        "metadata hard limit must reject in-flight admission atomically");
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: prepared_harfbuzz_face_cache_tests FONT\n";
        return 2;
    }
    const std::filesystem::path font_path(argv[1]);
    std::error_code filesystem_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(font_path, filesystem_error);
    if (filesystem_error || file_size == 0U ||
        file_size > static_cast<std::uintmax_t>(
                        std::numeric_limits<std::size_t>::max() / 6U)) {
        return 2;
    }
    const std::size_t font_size = static_cast<std::size_t>(file_size);
    bool ok = true;
    ok &= cold_hit_clear_lifetime(font_path, font_size);
    ok &= concurrent_single_flight(font_path, font_size);
    ok &= deterministic_lru(font_path, font_size);
    ok &= limits_and_failures_are_atomic(font_path, font_size);
    if (!ok) {
        return 1;
    }
    std::cout << "prepared HarfBuzz face cache tests passed\n";
    return 0;
}
