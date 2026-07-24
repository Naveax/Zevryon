#include "font_file_loader.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace zevryon::text;
using Bytes = std::vector<std::byte>;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
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
    put_record(&bytes, 12U, sfnt_tag('c', 'm', 'a', 'p'),
               checksum(all.subspan(kCmapOffset, 5U)), 64U, 5U);
    put_record(&bytes, 28U, sfnt_tag('h', 'e', 'a', 'd'),
               checksum_head(all.subspan(kHeadOffset, 12U)), 72U, 12U);
    put_record(&bytes, 44U, sfnt_tag('m', 'a', 'x', 'p'),
               checksum(all.subspan(kMaxpOffset, 6U)), 84U, 6U);
    put_u32(&bytes, kHeadOffset + 8U,
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
            ("zevryon-font-loader-" + std::to_string(stamp));
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
    if (!bytes.empty()) {
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    return stream.good();
}

bool successful_load_and_cache_hit(const TemporaryDirectory& temporary) {
    const Bytes font = make_font();
    const std::filesystem::path path = temporary.path / "valid.ttf";
    if (!write_file(path, font)) {
        return false;
    }

    VerifiedFontResourceCache cache(font.size() * 3U, 64U * 1024U, 3U);
    std::shared_ptr<const VerifiedFontResource> first;
    FontFileLoadStats first_stats;
    FontFileLoadError first_error;
    bool ok = expect(load_verified_font_file(
                         path,
                         0U,
                         font.size(),
                         &cache,
                         &first,
                         &first_stats,
                         &first_error),
                     "valid exact-limit font file must load");
    ok &= expect(first && first->bytes().size() == font.size(),
                 "successful load must publish retained bytes");
    ok &= expect(first_stats.file_bytes_before == font.size() &&
                     first_stats.file_bytes_after == font.size() &&
                     first_stats.exact_read_bytes == font.size() &&
                     first_stats.metadata_stable,
                 "file metadata and exact read counts must be recorded");
    ok &= expect(first_stats.staging.current_bytes == 0U &&
                     first_stats.staging.peak_bytes == font.size() &&
                     first_stats.staging.physical_read_bytes == font.size() &&
                     first_stats.staging.accounting_errors == 0U,
                 "staging bytes must be exact and released before return");
    ok &= expect(first_stats.cache.build_attempts == 1U &&
                     first_stats.cache.builds_published == 1U,
                 "first file load must publish one verified cache entry");

    std::shared_ptr<const VerifiedFontResource> repeated;
    FontFileLoadStats repeated_stats;
    FontFileLoadError repeated_error;
    ok &= expect(load_verified_font_file(
                         path,
                         0U,
                         font.size(),
                         &cache,
                         &repeated,
                         &repeated_stats,
                         &repeated_error),
                 "repeated file load must succeed");
    ok &= expect(repeated == first &&
                     repeated_stats.identity == first_stats.identity &&
                     repeated_stats.cache.build_attempts == 1U &&
                     repeated_stats.cache.hits == 1U,
                 "repeated stable file must reuse the identical handle");
    ok &= expect(repeated_stats.staging.current_bytes == 0U &&
                     repeated_stats.staging.peak_bytes == font.size(),
                 "cache hit file staging must also release completely");
    return ok;
}

bool path_and_limit_failures(const TemporaryDirectory& temporary) {
    const Bytes font = make_font();
    const std::filesystem::path valid_path = temporary.path / "limit.ttf";
    const std::filesystem::path empty_path = temporary.path / "empty.ttf";
    const std::filesystem::path missing_path = temporary.path / "missing.ttf";
    if (!write_file(valid_path, font) || !write_file(empty_path, {})) {
        return false;
    }

    VerifiedFontResourceCache cache(font.size() * 2U, 64U * 1024U, 2U);
    std::shared_ptr<const VerifiedFontResource> output;
    FontFileLoadStats stats;
    FontFileLoadError error;

    bool ok = expect(!load_verified_font_file(
                         valid_path,
                         0U,
                         font.size() - 1U,
                         &cache,
                         &output,
                         &stats,
                         &error),
                     "font above staging limit must fail");
    ok &= expect(!output && error.kind == FontFileLoadErrorKind::FileTooLarge &&
                     stats.exact_read_bytes == 0U,
                 "oversize rejection must happen before physical read");

    ok &= expect(!load_verified_font_file(
                         missing_path,
                         0U,
                         font.size(),
                         &cache,
                         &output,
                         &stats,
                         &error),
                 "missing path must fail");
    ok &= expect(!output && error.kind == FontFileLoadErrorKind::MetadataFailed,
                 "missing path must expose metadata failure");

    ok &= expect(!load_verified_font_file(
                         temporary.path,
                         0U,
                         font.size(),
                         &cache,
                         &output,
                         &stats,
                         &error),
                 "directory path must fail");
    ok &= expect(!output && error.kind == FontFileLoadErrorKind::NotRegularFile,
                 "directory must report not-regular-file");

    ok &= expect(!load_verified_font_file(
                         empty_path,
                         0U,
                         font.size(),
                         &cache,
                         &output,
                         &stats,
                         &error),
                 "empty font file must fail");
    ok &= expect(!output && error.kind == FontFileLoadErrorKind::EmptyFile,
                 "empty file must report exact error");

    ok &= expect(!load_verified_font_file(
                         {},
                         0U,
                         font.size(),
                         &cache,
                         &output,
                         &stats,
                         &error),
                 "empty path argument must fail");
    ok &= expect(error.kind == FontFileLoadErrorKind::InvalidArgument,
                 "empty path must report invalid argument");
    return ok;
}

bool verification_failures_are_chained(const TemporaryDirectory& temporary) {
    const Bytes valid = make_font();
    Bytes corrupt = valid;
    corrupt[64U] ^= std::byte{1U};
    const std::filesystem::path corrupt_path = temporary.path / "corrupt.ttf";
    const std::filesystem::path valid_path = temporary.path / "face.ttf";
    if (!write_file(corrupt_path, corrupt) || !write_file(valid_path, valid)) {
        return false;
    }

    VerifiedFontResourceCache cache(valid.size() * 3U, 64U * 1024U, 3U);
    std::shared_ptr<const VerifiedFontResource> output;
    FontFileLoadStats stats;
    FontFileLoadError error;

    bool ok = expect(!load_verified_font_file(
                         corrupt_path,
                         0U,
                         corrupt.size(),
                         &cache,
                         &output,
                         &stats,
                         &error),
                     "corrupt font file must fail verification");
    ok &= expect(!output && error.kind == FontFileLoadErrorKind::CacheFailed &&
                     error.cache_error.kind ==
                         VerifiedFontResourceCacheErrorKind::ResourceBuildFailed &&
                     error.cache_error.resource_error ==
                         VerifiedFontResourceErrorKind::IntegrityFailure &&
                     error.cache_error.integrity_error ==
                         SfntIntegrityErrorKind::TableChecksumMismatch,
                 "checksum failure must preserve the complete error chain");
    ok &= expect(stats.staging.current_bytes == 0U &&
                     stats.staging.peak_bytes == corrupt.size(),
                 "failed verification must still release staging bytes");

    ok &= expect(!load_verified_font_file(
                         valid_path,
                         1U,
                         valid.size(),
                         &cache,
                         &output,
                         &stats,
                         &error),
                 "invalid face index must fail");
    ok &= expect(!output && error.kind == FontFileLoadErrorKind::CacheFailed &&
                     error.cache_error.resource_error ==
                         VerifiedFontResourceErrorKind::ParseFailure &&
                     error.cache_error.parse_error ==
                         SfntParseErrorKind::InvalidFaceIndex,
                 "face-index failure must preserve parser detail");
    return ok;
}

bool failure_clears_previous_output(const TemporaryDirectory& temporary) {
    const Bytes font = make_font();
    const std::filesystem::path path = temporary.path / "atomic.ttf";
    if (!write_file(path, font)) {
        return false;
    }
    VerifiedFontResourceCache cache(font.size() * 2U, 64U * 1024U, 2U);
    std::shared_ptr<const VerifiedFontResource> output;
    FontFileLoadStats stats;
    FontFileLoadError error;
    if (!load_verified_font_file(
            path, 0U, font.size(), &cache, &output, &stats, &error) ||
        !output) {
        return false;
    }
    const bool failed = !load_verified_font_file(
        temporary.path / "gone.ttf",
        0U,
        font.size(),
        &cache,
        &output,
        &stats,
        &error);
    return expect(failed && !output,
                  "failure must clear a previously populated output handle");
}

} // namespace

int main() {
    TemporaryDirectory temporary;
    bool ok = true;
    ok &= successful_load_and_cache_hit(temporary);
    ok &= path_and_limit_failures(temporary);
    ok &= verification_failures_are_chained(temporary);
    ok &= failure_clears_previous_output(temporary);
    if (!ok) {
        return 1;
    }
    std::cout << "font-file-loader-tests: PASS\n";
    return 0;
}
