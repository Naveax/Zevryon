#include "catalog_font_resource_resolver.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace zevryon::text;
using Bytes = std::vector<std::byte>;

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

std::string fontconfig_identity(
    std::string_view sysroot,
    std::string_view path,
    std::string_view face_index,
    std::string_view postscript) {
    std::string result = "fontconfig|";
    append_field(&result, sysroot);
    append_field(&result, path);
    append_field(&result, face_index);
    append_field(&result, postscript);
    append_field(&result, "");
    return result;
}

std::string directwrite_identity(
    std::initializer_list<std::string_view> paths,
    std::string_view face_index,
    std::string_view postscript) {
    std::string result = "directwrite|";
    append_field(&result, std::to_string(paths.size()));
    for (const std::string_view path : paths) {
        append_field(&result, path);
    }
    append_field(&result, "1");
    append_field(&result, face_index);
    append_field(&result, "400");
    append_field(&result, "5");
    append_field(&result, "0");
    append_field(&result, postscript);
    return result;
}

std::string coretext_identity(
    std::string_view path,
    std::string_view postscript) {
    std::string result = "coretext|";
    append_field(&result, path);
    append_field(&result, postscript);
    append_field(&result, "400");
    append_field(&result, "5");
    append_field(&result, "0");
    append_field(&result, "0");
    return result;
}

std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

void put_u16(Bytes* bytes, std::size_t offset, std::uint16_t value) {
    (*bytes)[offset] = std::byte{static_cast<unsigned char>(value >> 8U)};
    (*bytes)[offset + 1U] = std::byte{static_cast<unsigned char>(value)};
}

void put_u32(Bytes* bytes, std::size_t offset, std::uint32_t value) {
    (*bytes)[offset] = std::byte{static_cast<unsigned char>(value >> 24U)};
    (*bytes)[offset + 1U] =
        std::byte{static_cast<unsigned char>(value >> 16U)};
    (*bytes)[offset + 2U] =
        std::byte{static_cast<unsigned char>(value >> 8U)};
    (*bytes)[offset + 3U] = std::byte{static_cast<unsigned char>(value)};
}

std::uint32_t checksum(std::span<const std::byte> bytes) {
    std::uint32_t sum = 0U;
    for (std::size_t offset = 0U; offset < bytes.size(); offset += 4U) {
        std::uint32_t word = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            const std::size_t current = offset + index;
            const unsigned char value = current < bytes.size()
                ? std::to_integer<unsigned char>(bytes[current])
                : 0U;
            word |= static_cast<std::uint32_t>(value)
                << static_cast<unsigned int>((3U - index) * 8U);
        }
        sum += word;
    }
    return sum;
}

std::uint32_t checksum_head(std::span<const std::byte> bytes) {
    Bytes copy(bytes.begin(), bytes.end());
    if (copy.size() >= 12U) {
        put_u32(&copy, 8U, 0U);
    }
    return checksum(copy);
}

void put_record(
    Bytes* bytes,
    std::size_t offset,
    std::uint32_t tag,
    std::uint32_t table_checksum,
    std::uint32_t table_offset,
    std::uint32_t length) {
    put_u32(bytes, offset, tag);
    put_u32(bytes, offset + 4U, table_checksum);
    put_u32(bytes, offset + 8U, table_offset);
    put_u32(bytes, offset + 12U, length);
}

Bytes make_font() {
    constexpr std::size_t kCmapOffset = 64U;
    constexpr std::size_t kHeadOffset = 72U;
    constexpr std::size_t kMaxpOffset = 84U;
    Bytes bytes(92U, std::byte{0});
    put_u32(&bytes, 0U, 0x00010000U);
    put_u16(&bytes, 4U, 3U);
    put_u16(&bytes, 6U, 32U);
    put_u16(&bytes, 8U, 1U);
    put_u16(&bytes, 10U, 16U);
    for (std::size_t index = 0U; index < 5U; ++index) {
        bytes[kCmapOffset + index] =
            std::byte{static_cast<unsigned char>(0x10U + index)};
    }
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[kHeadOffset + index] =
            std::byte{static_cast<unsigned char>(0x20U + index)};
    }
    for (std::size_t index = 0U; index < 6U; ++index) {
        bytes[kMaxpOffset + index] =
            std::byte{static_cast<unsigned char>(0x30U + index)};
    }
    const auto all = std::span<const std::byte>(bytes);
    put_record(
        &bytes,
        12U,
        sfnt_tag('c', 'm', 'a', 'p'),
        checksum(all.subspan(kCmapOffset, 5U)),
        64U,
        5U);
    put_record(
        &bytes,
        28U,
        sfnt_tag('h', 'e', 'a', 'd'),
        checksum_head(all.subspan(kHeadOffset, 12U)),
        72U,
        12U);
    put_record(
        &bytes,
        44U,
        sfnt_tag('m', 'a', 'x', 'p'),
        checksum(all.subspan(kMaxpOffset, 6U)),
        84U,
        6U);
    put_u32(
        &bytes,
        kHeadOffset + 8U,
        kSfntWholeFontChecksum - checksum(bytes));
    return bytes;
}

struct TemporaryDirectory {
    std::filesystem::path path;

    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path = std::filesystem::temp_directory_path() /
            ("zevryon-catalog-resolver-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

bool write_file(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

struct GenerationFixture {
    std::array<FontCoverageRange, 1> coverage{{{0x0020U, 0x007eU}}};
    std::vector<std::string> identities;
    std::vector<std::string> families;
    std::vector<FontDiscoveryFace> faces;
    std::shared_ptr<const FontCatalogGeneration> generation;

    bool build(std::vector<std::string> values) {
        identities = std::move(values);
        families.clear();
        faces.clear();
        families.reserve(identities.size());
        faces.reserve(identities.size());
        for (std::size_t index = 0U; index < identities.size(); ++index) {
            families.push_back("Resolver Family " + std::to_string(index));
            faces.push_back(FontDiscoveryFace{
                identities[index],
                families[index],
                400U,
                5U,
                FontSlant::Upright,
                ScriptId::Latn,
                0U,
                coverage});
        }
        FontDiscoveryStats stats;
        FontDiscoveryError error;
        return build_font_catalog_generation(
            77U,
            faces,
            kDiscoveryLimit,
            kCatalogLimit,
            &generation,
            &stats,
            &error);
    }

    FontFaceId face_id(std::string_view identity) const {
        if (!generation) {
            return kInvalidFontFaceId;
        }
        for (std::size_t index = 0U;
             index < generation->discovery_records().size();
             ++index) {
            const FontFaceId candidate = static_cast<FontFaceId>(index);
            if (generation->identity(candidate) == identity) {
                return candidate;
            }
        }
        return kInvalidFontFaceId;
    }
};

bool direct_faces_share_content_resource(const TemporaryDirectory& temporary) {
    const Bytes font = make_font();
    const std::filesystem::path file = temporary.path / "shared.ttf";
    if (!write_file(file, font)) {
        return false;
    }
    const std::string utf8_path = path_utf8(file);
    const std::string fontconfig =
        fontconfig_identity("", utf8_path, "0", "SharedFC");
    const std::string directwrite =
        directwrite_identity({utf8_path}, "0", "SharedDW");

    GenerationFixture fixture;
    if (!fixture.build({fontconfig, directwrite})) {
        return false;
    }
    const FontFaceId fontconfig_face = fixture.face_id(fontconfig);
    const FontFaceId directwrite_face = fixture.face_id(directwrite);
    VerifiedFontResourceCache cache(font.size() * 3U, 64U * 1024U, 3U);

    std::shared_ptr<const VerifiedFontResource> first;
    CatalogFontResourceStats first_stats;
    CatalogFontResourceError first_error;
    bool ok = expect(
        resolve_catalog_font_resource(
            fixture.generation,
            fontconfig_face,
            font.size(),
            &cache,
            &first,
            &first_stats,
            &first_error),
        "Fontconfig catalog face must resolve");
    ok &= expect(
        first && first_stats.generation_id == 77U &&
            first_stats.face_id == fontconfig_face &&
            first_stats.platform_kind == FontPlatformIdentityKind::Fontconfig &&
            first_stats.capability ==
                FontLoadCapability::SingleFileWithFaceIndex,
        "Fontconfig resolver metadata must be exact");
    ok &= expect(
        first_stats.file_load.staging.current_bytes == 0U &&
            first_stats.file_load.cache.build_attempts == 1U,
        "first resolver load must release staging and build once");

    std::shared_ptr<const VerifiedFontResource> second;
    CatalogFontResourceStats second_stats;
    CatalogFontResourceError second_error;
    ok &= expect(
        resolve_catalog_font_resource(
            fixture.generation,
            directwrite_face,
            font.size(),
            &cache,
            &second,
            &second_stats,
            &second_error),
        "single-file DirectWrite catalog face must resolve");
    ok &= expect(
        second == first &&
            second_stats.platform_kind == FontPlatformIdentityKind::DirectWrite &&
            second_stats.file_load.cache.build_attempts == 1U &&
            second_stats.file_load.cache.hits == 1U,
        "different platform identities for the same bytes must share one handle");
    ok &= expect(
        second_stats.identity_bytes == directwrite.size() &&
            second_stats.path_bytes == utf8_path.size() &&
            second_stats.locator.file_count == 1U,
        "DirectWrite resolver locator statistics must be exact");
    return ok;
}

bool unsupported_capabilities_fail_before_io(
    const TemporaryDirectory& temporary) {
    const std::string utf8_path = path_utf8(temporary.path / "unused.ttf");
    const std::string sysroot =
        fontconfig_identity("/sdk-root", utf8_path, "0", "SysrootPS");
    const std::string multi = directwrite_identity(
        {utf8_path, utf8_path}, "0", "MultiPS");
    const std::string coretext = coretext_identity(utf8_path, "CorePS");
    const std::string malformed = "adapter/invalid";
    const std::string long_path(32769U, 'a');
    const std::string excessive =
        directwrite_identity({long_path}, "0", "LongPS");

    GenerationFixture fixture;
    if (!fixture.build({sysroot, multi, coretext, malformed, excessive})) {
        return false;
    }
    VerifiedFontResourceCache cache(1024U * 1024U, 64U * 1024U, 8U);
    std::shared_ptr<const VerifiedFontResource> output;
    CatalogFontResourceStats stats;
    CatalogFontResourceError error;

    bool ok = expect(
        !resolve_catalog_font_resource(
            fixture.generation,
            fixture.face_id(sysroot),
            1024U,
            &cache,
            &output,
            &stats,
            &error),
        "non-empty Fontconfig sysroot must fail closed");
    ok &= expect(
        !output &&
            error.kind ==
                CatalogFontResourceErrorKind::FontconfigSysrootUnsupported &&
            stats.file_load.exact_read_bytes == 0U,
        "sysroot rejection must occur before file I/O");

    ok &= expect(
        !resolve_catalog_font_resource(
            fixture.generation,
            fixture.face_id(multi),
            1024U,
            &cache,
            &output,
            &stats,
            &error),
        "multi-file DirectWrite face must fail closed");
    ok &= expect(
        !output &&
            error.kind == CatalogFontResourceErrorKind::MultiFileUnsupported &&
            stats.capability == FontLoadCapability::MultiFile &&
            stats.file_load.exact_read_bytes == 0U,
        "multi-file failure must preserve capability and avoid I/O");

    ok &= expect(
        !resolve_catalog_font_resource(
            fixture.generation,
            fixture.face_id(coretext),
            1024U,
            &cache,
            &output,
            &stats,
            &error),
        "CoreText unresolved face index must fail closed");
    ok &= expect(
        !output &&
            error.kind == CatalogFontResourceErrorKind::FaceIndexUnresolved &&
            stats.capability ==
                FontLoadCapability::SingleFileFaceIndexUnresolved,
        "CoreText failure must preserve unresolved capability");

    ok &= expect(
        !resolve_catalog_font_resource(
            fixture.generation,
            fixture.face_id(malformed),
            1024U,
            &cache,
            &output,
            &stats,
            &error),
        "malformed platform identity must fail");
    ok &= expect(
        !output &&
            error.kind == CatalogFontResourceErrorKind::IdentityParseFailed &&
            error.locator_error.kind ==
                FontLoadLocatorErrorKind::UnsupportedPrefix,
        "identity parse error must be fully chained");

    ok &= expect(
        !resolve_catalog_font_resource(
            fixture.generation,
            fixture.face_id(excessive),
            1024U,
            &cache,
            &output,
            &stats,
            &error),
        "excessive portable path must fail");
    ok &= expect(
        !output && error.kind == CatalogFontResourceErrorKind::PathTooLong &&
            stats.path_bytes == long_path.size() &&
            stats.file_load.exact_read_bytes == 0U,
        "path limit failure must occur before conversion and I/O");
    return ok;
}

bool seed_valid_output(
    std::shared_ptr<const VerifiedFontResource>* output) {
    const Bytes font = make_font();
    VerifiedFontResourceStats stats;
    VerifiedFontResourceError error;
    return build_verified_font_resource(
        999U,
        font,
        0U,
        font.size() * 2U,
        output,
        &stats,
        &error);
}

bool nested_file_failure_and_atomic_output(const TemporaryDirectory& temporary) {
    const std::filesystem::path missing = temporary.path / "missing.ttf";
    const std::string identity =
        directwrite_identity({path_utf8(missing)}, "0", "MissingPS");
    GenerationFixture fixture;
    if (!fixture.build({identity})) {
        return false;
    }
    VerifiedFontResourceCache cache(1024U * 1024U, 64U * 1024U, 2U);
    std::shared_ptr<const VerifiedFontResource> output;
    CatalogFontResourceStats stats;
    CatalogFontResourceError error;

    bool ok = expect(
        !resolve_catalog_font_resource(
            fixture.generation,
            fixture.face_id(identity),
            1024U,
            &cache,
            &output,
            &stats,
            &error),
        "missing catalog font file must fail");
    ok &= expect(
        !output && error.kind == CatalogFontResourceErrorKind::FileLoadFailed &&
            error.file_error.kind == FontFileLoadErrorKind::MetadataFailed,
        "file-loader failure must remain nested");

    ok &= expect(seed_valid_output(&output),
                 "test must seed output through public verified-resource API");
    ok &= expect(
        !resolve_catalog_font_resource(
            fixture.generation,
            kInvalidFontFaceId,
            1024U,
            &cache,
            &output,
            &stats,
            &error),
        "invalid face id must fail");
    ok &= expect(
        !output && error.kind == CatalogFontResourceErrorKind::InvalidFaceId,
        "invalid face failure must clear previous output");

    ok &= expect(
        !resolve_catalog_font_resource(
            nullptr,
            0U,
            1024U,
            &cache,
            &output,
            &stats,
            &error) &&
            error.kind == CatalogFontResourceErrorKind::InvalidArgument,
        "null generation must fail as invalid argument");
    return ok;
}

} // namespace

int main() {
    TemporaryDirectory temporary;
    bool ok = true;
    ok &= direct_faces_share_content_resource(temporary);
    ok &= unsupported_capabilities_fail_before_io(temporary);
    ok &= nested_file_failure_and_atomic_output(temporary);
    if (!ok) {
        return 1;
    }
    std::cout << "catalog-font-resource-resolver-tests: PASS\n";
    return 0;
}
