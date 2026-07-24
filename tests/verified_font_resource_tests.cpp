#include "verified_font_resource.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace {

namespace core = zevryon::core;
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
    (*bytes)[offset + 1U] = std::byte{static_cast<unsigned char>(value >> 16U)};
    (*bytes)[offset + 2U] = std::byte{static_cast<unsigned char>(value >> 8U)};
    (*bytes)[offset + 3U] = std::byte{static_cast<unsigned char>(value)};
}

void put_record(
    Bytes* bytes,
    std::size_t offset,
    std::uint32_t tag,
    std::uint32_t checksum_value,
    std::uint32_t table_offset,
    std::uint32_t length) {
    put_u32(bytes, offset, tag);
    put_u32(bytes, offset + 4U, checksum_value);
    put_u32(bytes, offset + 8U, table_offset);
    put_u32(bytes, offset + 12U, length);
}

std::uint32_t checksum(
    std::span<const std::byte> bytes,
    std::size_t zero_offset = 0U,
    std::size_t zero_length = 0U) {
    std::uint32_t sum = 0U;
    for (std::size_t offset = 0U; offset < bytes.size(); offset += 4U) {
        std::uint32_t word = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            const std::size_t current = offset + index;
            unsigned char value = 0U;
            if (current < bytes.size()) {
                const bool zeroed = zero_length != 0U && current >= zero_offset &&
                    current - zero_offset < zero_length;
                if (!zeroed) {
                    value = std::to_integer<unsigned char>(bytes[current]);
                }
            }
            word |= static_cast<std::uint32_t>(value)
                    << static_cast<unsigned int>((3U - index) * 8U);
        }
        sum += word;
    }
    return sum;
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
               checksum(all.subspan(kHeadOffset, 12U), 8U, 4U), 72U, 12U);
    put_record(&bytes, 44U, sfnt_tag('m', 'a', 'x', 'p'),
               checksum(all.subspan(kMaxpOffset, 6U)), 84U, 6U);
    put_u32(&bytes, kHeadOffset + 8U, kSfntWholeFontChecksum - checksum(bytes));
    return bytes;
}

bool exact_retention_and_lifetime() {
    Bytes source = make_font();
    const Bytes original = source;
    std::shared_ptr<const VerifiedFontResource> resource;
    VerifiedFontResourceStats stats;
    VerifiedFontResourceError error;
    bool ok = expect(
        build_verified_font_resource(
            41U, source, 0U, source.size(), &resource, &stats, &error),
        "valid font must build at the exact byte hard limit");
    if (!resource) {
        return false;
    }

    ok &= expect(resource->resource_id() == 41U,
                 "resource identity must be retained");
    ok &= expect(resource->bytes().size() == original.size() &&
                     resource->bytes().data() == resource->view().bytes().data(),
                 "view must address retained immutable storage");
    ok &= expect(stats.source_bytes == original.size() &&
                     stats.retained_bytes == original.size() &&
                     stats.parse.table_count == 3U &&
                     stats.integrity.checksums_verified == 3U,
                 "build statistics must preserve validation evidence");
    const core::ResourceSnapshot snapshot = resource->resource_snapshot();
    ok &= expect(snapshot.current_bytes == original.size() &&
                     snapshot.peak_bytes == original.size() &&
                     snapshot.reservations == 1U &&
                     snapshot.rejected_reservations == 0U,
                 "font byte accounting must be exact");
    ok &= expect(resource->accounting_clean() && resource->within_hard_limit(),
                 "retained resource ledger must remain clean and bounded");

    source[0U] ^= std::byte{0xffU};
    source.clear();
    ok &= expect(resource->bytes().front() == original.front(),
                 "caller mutation and release must not affect retained bytes");
    SfntTableRecord head;
    ok &= expect(resource->view().find_table(sfnt_tag('h', 'e', 'a', 'd'), &head),
                 "retained view must survive caller storage changes");

    std::shared_ptr<const VerifiedFontResource> reader = resource;
    std::weak_ptr<const VerifiedFontResource> weak = resource;
    resource.reset();
    ok &= expect(!weak.expired() && reader->view().valid(),
                 "reader snapshot must retain the immutable resource");
    reader.reset();
    ok &= expect(weak.expired(),
                 "resource must release after its final reader");
    return ok;
}

bool atomic_failures_preserve_other_readers() {
    Bytes valid = make_font();
    std::shared_ptr<const VerifiedFontResource> output;
    bool ok = expect(
        build_verified_font_resource(
            7U, valid, 0U, valid.size(), &output, nullptr, nullptr),
        "reader-isolation fixture must build");
    std::shared_ptr<const VerifiedFontResource> retained = output;

    const Bytes invalid(12U, std::byte{0});
    VerifiedFontResourceError error;
    VerifiedFontResourceStats stats;
    ok &= expect(
        !build_verified_font_resource(
            8U, invalid, 0U, invalid.size(), &output, &stats, &error),
        "structurally invalid replacement must fail");
    ok &= expect(!output && error.kind == VerifiedFontResourceErrorKind::ParseFailed &&
                     error.parse_error == SfntParseErrorKind::UnsupportedSfntVersion &&
                     stats.retained_bytes == 0U && retained->view().valid(),
                 "parse failure must be atomic and preserve an existing reader");

    Bytes corrupt = valid;
    corrupt[64U] ^= std::byte{1U};
    ok &= expect(
        !build_verified_font_resource(
            9U, corrupt, 0U, corrupt.size(), &output, &stats, &error),
        "checksum-corrupt replacement must fail");
    ok &= expect(!output &&
                     error.kind == VerifiedFontResourceErrorKind::IntegrityFailed &&
                     error.integrity_error ==
                         SfntIntegrityErrorKind::TableChecksumMismatch &&
                     error.table_tag == sfnt_tag('c', 'm', 'a', 'p') &&
                     retained->bytes().size() == valid.size(),
                 "integrity failure must be exact and reader-safe");
    return ok;
}

bool budget_and_argument_failures() {
    Bytes source = make_font();
    std::shared_ptr<const VerifiedFontResource> output;
    VerifiedFontResourceError error;
    bool ok = expect(
        !build_verified_font_resource(
            11U, source, 0U, source.size() - 1U, &output, nullptr, &error),
        "source above retained-byte hard limit must fail before allocation");
    ok &= expect(!output &&
                     error.kind ==
                         VerifiedFontResourceErrorKind::OutputBudgetExceeded &&
                     error.byte_offset == source.size(),
                 "budget failure must report the exact source size");
    ok &= expect(
        !build_verified_font_resource(
            0U, source, 0U, source.size(), &output, nullptr, &error),
        "zero resource identity must fail");
    ok &= expect(error.kind == VerifiedFontResourceErrorKind::InvalidArgument,
                 "zero resource identity must report invalid argument");
    ok &= expect(
        !build_verified_font_resource(
            12U, source, 1U, source.size(), &output, nullptr, &error),
        "out-of-range face index must fail after retention");
    ok &= expect(!output && error.kind == VerifiedFontResourceErrorKind::ParseFailed &&
                     error.parse_error == SfntParseErrorKind::InvalidFaceIndex,
                 "face-index failure must preserve parser detail");
    ok &= expect(
        !build_verified_font_resource(
            13U, source, 0U, source.size(), nullptr, nullptr, &error),
        "null output pointer must fail");
    ok &= expect(error.kind == VerifiedFontResourceErrorKind::InvalidArgument &&
                     std::string_view(
                         verified_font_resource_error_kind_name(error.kind)) ==
                         "invalid_argument",
                 "argument error naming must remain stable");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= exact_retention_and_lifetime();
    ok &= atomic_failures_preserve_other_readers();
    ok &= budget_and_argument_failures();
    if (!ok) {
        return 1;
    }
    std::cout << "verified-font-resource-tests: PASS\n";
    return 0;
}
