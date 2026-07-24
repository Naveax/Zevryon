#include "prepared_harfbuzz_face.hpp"

#include "resource_ledger.hpp"
#include "unicode_script.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using namespace zevryon::core;
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

std::string fontconfig_identity(const std::filesystem::path& path) {
    std::string result = "fontconfig|";
    append_field(&result, "");
    append_field(&result, path_utf8(path));
    append_field(&result, "0");
    append_field(&result, "PreparedFacePS");
    append_field(&result, "");
    return result;
}

ScriptId latin_script() {
    ScriptId script = ScriptId::Zzzz;
    (void)script_id_from_name("Latn", &script);
    return script;
}

struct GenerationFixture {
    std::string identity;
    std::string family{"Prepared Face Family"};
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

bool prepare_after_cache_clear(
    const std::filesystem::path& path,
    std::size_t font_size) {
    GenerationFixture generation;
    if (!generation.build(path, 910U)) {
        return false;
    }
    VerifiedFontResourceCache cache(font_size * 3U, 64U * 1024U, 4U);
    CatalogFontFaceBinding binding;
    CatalogFontResourceStats binding_stats;
    CatalogFontResourceError binding_error;
    bool ok = expect(
        bind_catalog_font_face(
            generation.generation,
            0U,
            font_size * 2U,
            &cache,
            &binding,
            &binding_stats,
            &binding_error),
        "real catalog face must bind");
    if (!binding.valid()) {
        return false;
    }

    generation.generation.reset();
    cache.clear();
    const VerifiedFontResourceCacheStats cache_before = cache.snapshot();

    std::shared_ptr<const PreparedHarfBuzzFace> prepared;
    PreparedHarfBuzzFaceStats stats;
    PreparedHarfBuzzFaceError error;
    ok &= expect(
        prepare_harfbuzz_face(binding, &prepared, &stats, &error),
        "binding must prepare an immutable HarfBuzz face after cache clear");
    const VerifiedFontResourceCacheStats cache_after = cache.snapshot();
    ok &= expect(
        same_cache_snapshot(cache_before, cache_after),
        "prepared-face construction must not touch the verified-resource cache");
    ok &= expect(
        prepared && prepared->valid() &&
            prepared->generation_id() == 910U &&
            prepared->face_id() == 0U &&
            prepared->resource_id() == binding.resource_id() &&
            prepared->font_bytes() == font_size &&
            prepared->glyph_count() > 0U &&
            prepared->units_per_em() > 0U &&
            prepared_harfbuzz_native_face(*prepared) != nullptr,
        "prepared face must expose exact retained identity and native metadata");
    ok &= expect(
        stats.generation_id == 910U && stats.face_id == 0U &&
            stats.resource_id == prepared->resource_id() &&
            stats.font_bytes == font_size &&
            stats.glyph_count == prepared->glyph_count() &&
            stats.units_per_em == prepared->units_per_em() &&
            stats.blob_created && stats.face_created && stats.face_immutable,
        "preparation statistics must certify blob and immutable face creation");

    const std::uint64_t resource_id = prepared->resource_id();
    binding = CatalogFontFaceBinding{};
    ok &= expect(
        prepared->valid() && prepared->resource_id() == resource_id &&
            prepared->binding().valid(),
        "prepared face must retain the binding after caller release");
    return ok;
}

bool failed_prepare_is_atomic(
    const std::filesystem::path& path,
    std::size_t font_size) {
    GenerationFixture generation;
    if (!generation.build(path, 911U)) {
        return false;
    }
    VerifiedFontResourceCache cache(font_size * 3U, 64U * 1024U, 4U);
    CatalogFontFaceBinding binding;
    CatalogFontResourceStats binding_stats;
    CatalogFontResourceError binding_error;
    if (!bind_catalog_font_face(
            generation.generation,
            0U,
            font_size * 2U,
            &cache,
            &binding,
            &binding_stats,
            &binding_error)) {
        return false;
    }

    std::shared_ptr<const PreparedHarfBuzzFace> output;
    PreparedHarfBuzzFaceStats stats;
    PreparedHarfBuzzFaceError error;
    if (!prepare_harfbuzz_face(binding, &output, &stats, &error)) {
        return false;
    }
    CatalogFontFaceBinding invalid;
    const bool result = prepare_harfbuzz_face(
        invalid,
        &output,
        &stats,
        &error);
    return expect(!result, "default binding preparation must fail") &&
        expect(!output, "failed preparation must clear previous output") &&
        expect(error.kind == PreparedHarfBuzzFaceErrorKind::InvalidBinding,
               "failed preparation must report invalid binding") &&
        expect(!stats.blob_created && !stats.face_created,
               "failed preparation must publish no native creation evidence");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: prepared_harfbuzz_face_tests FONT\n";
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
    const std::size_t font_size = static_cast<std::size_t>(file_size);
    bool ok = true;
    ok &= prepare_after_cache_clear(font_path, font_size);
    ok &= failed_prepare_is_atomic(font_path, font_size);
    if (!ok) {
        return 1;
    }
    std::cout << "prepared HarfBuzz face tests passed\n";
    return 0;
}
