#include "font_content_identity.hpp"
#include "verified_font_resource_cache.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
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

unsigned char hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned char>(value - 'a' + 10);
    }
    return 0xffU;
}

Sha256Digest digest_from_hex(std::string_view hex) {
    Sha256Digest result{};
    if (hex.size() != result.size() * 2U) {
        return result;
    }
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const unsigned char high = hex_digit(hex[index * 2U]);
        const unsigned char low = hex_digit(hex[index * 2U + 1U]);
        result[index] = std::byte{
            static_cast<unsigned char>((high << 4U) | low)};
    }
    return result;
}

std::span<const std::byte> as_bytes(std::string_view value) {
    return std::as_bytes(std::span(value.data(), value.size()));
}

bool hash_text(
    std::string_view text,
    std::size_t chunk_size,
    Sha256Digest* output) {
    Sha256 hash;
    const auto bytes = as_bytes(text);
    if (chunk_size == 0U) {
        return hash.update(bytes) && hash.finish(output);
    }
    for (std::size_t offset = 0U; offset < bytes.size();) {
        const std::size_t remaining = bytes.size() - offset;
        const std::size_t count =
            remaining < chunk_size ? remaining : chunk_size;
        if (!hash.update(bytes.subspan(offset, count))) {
            return false;
        }
        offset += count;
    }
    return hash.finish(output);
}

bool standard_sha256_vectors() {
    constexpr std::string_view kLong =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const Sha256Digest empty_expected = digest_from_hex(
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855");
    const Sha256Digest abc_expected = digest_from_hex(
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");
    const Sha256Digest long_expected = digest_from_hex(
        "248d6a61d20638b8e5c026930c3e6039"
        "a33ce45964ff2167f6ecedd419db06c1");

    Sha256Digest digest{};
    bool ok = expect(hash_text({}, 0U, &digest) && digest == empty_expected,
                     "empty SHA-256 vector must match");
    ok &= expect(hash_text("abc", 0U, &digest) && digest == abc_expected,
                 "abc SHA-256 vector must match");
    ok &= expect(hash_text("abc", 1U, &digest) && digest == abc_expected,
                 "single-byte chunking must not change SHA-256");
    ok &= expect(hash_text(kLong, 7U, &digest) && digest == long_expected,
                 "multi-block chunking must match the standard vector");

    std::array<std::byte, 1000> thousand_a{};
    thousand_a.fill(std::byte{static_cast<unsigned char>('a')});
    Sha256 million;
    for (std::size_t index = 0U; index < 1000U; ++index) {
        ok &= expect(million.update(thousand_a),
                     "million-a streaming update must succeed");
    }
    const Sha256Digest million_expected = digest_from_hex(
        "cdc76e5c9914fb9281a1c7e284d73e67"
        "f1809a48a497200e046d39ccc7112cd0");
    ok &= expect(million.finish(&digest) && digest == million_expected,
                 "million-a SHA-256 vector must match");
    ok &= expect(million.total_bytes() == 1000000U && million.finished(),
                 "SHA-256 state must report exact finalized length");
    ok &= expect(!million.update(thousand_a) && !million.finish(&digest),
                 "finalized SHA-256 state must reject reuse until reset");
    million.reset();
    ok &= expect(million.total_bytes() == 0U && !million.finished(),
                 "SHA-256 reset must restore initial state");
    return ok;
}

bool identity_domain_and_framing() {
    const auto abc = as_bytes("abc");
    const auto abcd = as_bytes("abcd");
    FontContentIdentity first;
    FontContentIdentity repeated;
    FontContentIdentity other_face;
    FontContentIdentity other_length;

    bool ok = expect(compute_font_content_identity(abc, 0U, &first),
                     "font identity must compute");
    ok &= expect(compute_font_content_identity(abc, 0U, &repeated) &&
                     repeated == first,
                 "font identity must be deterministic");
    ok &= expect(first.high == 0x28577325c3d1ceebULL &&
                     first.low == 0x4053887c8e190119ULL &&
                     first.face_index == 0U,
                 "font identity must match the pinned domain-separated vector");
    ok &= expect(compute_font_content_identity(abc, 1U, &other_face) &&
                     other_face != first && other_face.face_index == 1U,
                 "face index must be part of the identity framing");
    ok &= expect(compute_font_content_identity(abcd, 0U, &other_length) &&
                     other_length != first,
                 "byte length and payload must affect the identity");
    ok &= expect(!compute_font_content_identity(abc, 0U, nullptr),
                 "null identity output must fail");

    const VerifiedFontResourceCacheKey key = verified_font_cache_key(first);
    ok &= expect(key.high == first.high && key.low == first.low &&
                     key.face_index == first.face_index,
                 "cache-key conversion must preserve all identity fields");
    return ok;
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

bool content_addressed_cache_reuse() {
    const Bytes font = make_font();
    VerifiedFontResourceCache cache(font.size() * 2U, 64U * 1024U, 2U);
    std::shared_ptr<const VerifiedFontResource> first;
    std::shared_ptr<const VerifiedFontResource> repeated;
    FontContentIdentity first_identity;
    FontContentIdentity repeated_identity;
    VerifiedFontResourceCacheStats stats;
    VerifiedFontResourceCacheError error;

    bool ok = expect(cache.get_or_build_content_addressed(
                         font, 0U, &first, &first_identity, &stats, &error),
                     "content-addressed cache must build a valid font");
    ok &= expect(first && stats.build_attempts == 1U &&
                     stats.builds_published == 1U,
                 "first content-addressed request must publish one resource");
    ok &= expect(cache.get_or_build_content_addressed(
                         font, 0U, &repeated, &repeated_identity, &stats, &error),
                 "repeated content-addressed request must hit");
    ok &= expect(repeated == first && repeated_identity == first_identity &&
                     stats.build_attempts == 1U && stats.hits == 1U,
                 "same bytes and face must reuse the identical handle");

    Bytes changed = font;
    changed[64U] ^= std::byte{1U};
    FontContentIdentity changed_identity;
    ok &= expect(compute_font_content_identity(changed, 0U, &changed_identity) &&
                     changed_identity != first_identity,
                 "one-byte payload mutation must change the cache identity");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= standard_sha256_vectors();
    ok &= identity_domain_and_framing();
    ok &= content_addressed_cache_reuse();
    if (!ok) {
        return 1;
    }
    std::cout << "font-content-identity-tests: PASS\n";
    return 0;
}
