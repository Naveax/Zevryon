#include "font_resource_integrity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
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
    (*bytes)[offset + 1U] = std::byte{static_cast<unsigned char>(value >> 16U)};
    (*bytes)[offset + 2U] = std::byte{static_cast<unsigned char>(value >> 8U)};
    (*bytes)[offset + 3U] = std::byte{static_cast<unsigned char>(value)};
}

void put_record(
    Bytes* bytes,
    std::size_t offset,
    std::uint32_t tag,
    std::uint32_t checksum,
    std::uint32_t table_offset,
    std::uint32_t length) {
    put_u32(bytes, offset, tag);
    put_u32(bytes, offset + 4U, checksum);
    put_u32(bytes, offset + 8U, table_offset);
    put_u32(bytes, offset + 12U, length);
}

std::uint32_t independent_checksum(
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

Bytes make_valid_single_font() {
    constexpr std::size_t kCmapOffset = 64U;
    constexpr std::size_t kCmapLength = 5U;
    constexpr std::size_t kHeadOffset = 72U;
    constexpr std::size_t kHeadLength = 12U;
    constexpr std::size_t kMaxpOffset = 84U;
    constexpr std::size_t kMaxpLength = 6U;

    Bytes bytes(92U, std::byte{0});
    put_u32(&bytes, 0U, 0x00010000U);
    put_u16(&bytes, 4U, 3U);
    put_u16(&bytes, 6U, 32U);
    put_u16(&bytes, 8U, 1U);
    put_u16(&bytes, 10U, 16U);

    for (std::size_t index = 0U; index < kCmapLength; ++index) {
        bytes[kCmapOffset + index] =
            std::byte{static_cast<unsigned char>(0x10U + index)};
    }
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[kHeadOffset + index] =
            std::byte{static_cast<unsigned char>(0x20U + index)};
    }
    for (std::size_t index = 0U; index < kMaxpLength; ++index) {
        bytes[kMaxpOffset + index] =
            std::byte{static_cast<unsigned char>(0x30U + index)};
    }

    const std::uint32_t cmap_checksum = independent_checksum(
        std::span<const std::byte>(bytes).subspan(kCmapOffset, kCmapLength));
    const std::uint32_t head_checksum = independent_checksum(
        std::span<const std::byte>(bytes).subspan(kHeadOffset, kHeadLength),
        8U,
        4U);
    const std::uint32_t maxp_checksum = independent_checksum(
        std::span<const std::byte>(bytes).subspan(kMaxpOffset, kMaxpLength));

    put_record(&bytes, 12U, sfnt_tag('c', 'm', 'a', 'p'), cmap_checksum,
               static_cast<std::uint32_t>(kCmapOffset),
               static_cast<std::uint32_t>(kCmapLength));
    put_record(&bytes, 28U, sfnt_tag('h', 'e', 'a', 'd'), head_checksum,
               static_cast<std::uint32_t>(kHeadOffset),
               static_cast<std::uint32_t>(kHeadLength));
    put_record(&bytes, 44U, sfnt_tag('m', 'a', 'x', 'p'), maxp_checksum,
               static_cast<std::uint32_t>(kMaxpOffset),
               static_cast<std::uint32_t>(kMaxpLength));

    const std::uint32_t adjustment =
        kSfntWholeFontChecksum - independent_checksum(bytes);
    put_u32(&bytes, kHeadOffset + 8U, adjustment);
    return bytes;
}

Bytes make_valid_collection() {
    constexpr std::size_t kDirectoryOffset = 16U;
    constexpr std::size_t kCmapOffset = 64U;
    constexpr std::size_t kHeadOffset = 68U;
    constexpr std::size_t kHeadLength = 12U;

    Bytes bytes(80U, std::byte{0});
    put_u32(&bytes, 0U, sfnt_tag('t', 't', 'c', 'f'));
    put_u32(&bytes, 4U, 0x00010000U);
    put_u32(&bytes, 8U, 1U);
    put_u32(&bytes, 12U, static_cast<std::uint32_t>(kDirectoryOffset));

    put_u32(&bytes, kDirectoryOffset, 0x00010000U);
    put_u16(&bytes, kDirectoryOffset + 4U, 2U);
    put_u16(&bytes, kDirectoryOffset + 6U, 32U);
    put_u16(&bytes, kDirectoryOffset + 8U, 1U);
    put_u16(&bytes, kDirectoryOffset + 10U, 0U);

    bytes[kCmapOffset] = std::byte{0x12U};
    bytes[kCmapOffset + 1U] = std::byte{0x34U};
    bytes[kCmapOffset + 2U] = std::byte{0x56U};
    bytes[kCmapOffset + 3U] = std::byte{0x78U};
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[kHeadOffset + index] =
            std::byte{static_cast<unsigned char>(0x40U + index)};
    }
    put_u32(&bytes, kHeadOffset + 8U, 0xDEADBEEFU);

    const std::uint32_t cmap_checksum = independent_checksum(
        std::span<const std::byte>(bytes).subspan(kCmapOffset, 4U));
    const std::uint32_t head_checksum = independent_checksum(
        std::span<const std::byte>(bytes).subspan(kHeadOffset, kHeadLength),
        8U,
        4U);
    put_record(&bytes, kDirectoryOffset + 12U,
               sfnt_tag('c', 'm', 'a', 'p'), cmap_checksum,
               static_cast<std::uint32_t>(kCmapOffset), 4U);
    put_record(&bytes, kDirectoryOffset + 28U,
               sfnt_tag('h', 'e', 'a', 'd'), head_checksum,
               static_cast<std::uint32_t>(kHeadOffset),
               static_cast<std::uint32_t>(kHeadLength));
    return bytes;
}

bool open_view(const Bytes& bytes, SfntResourceView* view) {
    SfntParseError error;
    return open_sfnt_resource(bytes, 0U, view, nullptr, &error);
}

bool valid_single_font_integrity() {
    const Bytes bytes = make_valid_single_font();
    SfntResourceView view;
    bool ok = expect(open_view(bytes, &view),
                     "valid integrity fixture must pass the container parser");
    SfntIntegrityStats stats;
    SfntIntegrityError error;
    ok &= expect(verify_sfnt_integrity(view, {}, &stats, &error),
                 "valid single font must pass strict integrity verification");
    ok &= expect(stats.tables_seen == 3U && stats.aligned_tables == 3U &&
                     stats.checksums_verified == 3U,
                 "single-font table statistics must be exact");
    ok &= expect(stats.payload_bytes_checked == 23U &&
                     stats.padding_bytes_checked == 5U,
                 "payload and padding accounting must be exact");
    ok &= expect(stats.head_table_present &&
                     stats.whole_font_checksum_verified &&
                     !stats.whole_font_checksum_ignored_for_collection,
                 "single-font head and whole checksum state must be exact");
    ok &= expect(calculate_sfnt_checksum(
                     std::array<std::byte, 3U>{
                         std::byte{0x12U}, std::byte{0x34U}, std::byte{0x56U}}) ==
                     0x12345600U,
                 "checksum must pad a final partial word with zeros");
    return ok;
}

bool valid_collection_ignores_whole_adjustment() {
    const Bytes bytes = make_valid_collection();
    SfntResourceView view;
    bool ok = expect(open_view(bytes, &view),
                     "valid TTC fixture must pass the container parser");
    SfntIntegrityStats stats;
    ok &= expect(verify_sfnt_integrity(view, {}, &stats, nullptr),
                 "valid TTC must pass table integrity checks");
    ok &= expect(stats.tables_seen == 2U && stats.checksums_verified == 2U,
                 "TTC table checksums must be verified");
    ok &= expect(!stats.whole_font_checksum_verified &&
                     stats.whole_font_checksum_ignored_for_collection,
                 "TTC whole-font adjustment must be explicitly ignored");
    return ok;
}

bool alignment_and_padding_failures() {
    bool ok = true;
    SfntIntegrityError error;
    SfntResourceView view;

    Bytes misaligned = make_valid_single_font();
    misaligned.resize(93U, std::byte{0});
    for (std::size_t index = 0U; index < 6U; ++index) {
        misaligned[90U - index] = misaligned[89U - index];
    }
    misaligned[84U] = std::byte{0};
    put_u32(&misaligned, 52U, 85U);
    ok &= expect(open_view(misaligned, &view),
                 "misaligned table fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind == SfntIntegrityErrorKind::MisalignedTable,
                 "misaligned table must fail strict integrity policy");

    Bytes non_zero = make_valid_single_font();
    non_zero[69U] = std::byte{1U};
    ok &= expect(open_view(non_zero, &view),
                 "non-zero padding fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind == SfntIntegrityErrorKind::NonZeroPadding &&
                     error.byte_offset == 69U,
                 "first non-zero padding byte must be reported exactly");

    Bytes missing_padding = make_valid_single_font();
    missing_padding.resize(90U);
    ok &= expect(open_view(missing_padding, &view),
                 "missing terminal padding fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind == SfntIntegrityErrorKind::PaddingOutOfBounds,
                 "terminal padding outside the resource must fail");
    return ok;
}

bool checksum_failures() {
    bool ok = true;
    SfntIntegrityError error;
    SfntResourceView view;

    Bytes table_corruption = make_valid_single_font();
    table_corruption[64U] ^= std::byte{1U};
    ok &= expect(open_view(table_corruption, &view),
                 "table corruption fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind == SfntIntegrityErrorKind::TableChecksumMismatch &&
                     error.table_tag == sfnt_tag('c', 'm', 'a', 'p'),
                 "table payload corruption must report checksum mismatch");

    Bytes adjustment_corruption = make_valid_single_font();
    adjustment_corruption[80U] ^= std::byte{1U};
    ok &= expect(open_view(adjustment_corruption, &view),
                 "adjustment corruption fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind ==
                         SfntIntegrityErrorKind::WholeFontChecksumMismatch,
                 "head adjustment corruption must fail the whole-font checksum");
    return ok;
}

bool head_and_policy_behavior() {
    bool ok = true;
    SfntIntegrityError error;
    SfntResourceView view;

    Bytes missing_head = make_valid_single_font();
    put_u32(&missing_head, 28U, sfnt_tag('k', 'e', 'r', 'n'));
    ok &= expect(open_view(missing_head, &view),
                 "missing-head fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind == SfntIntegrityErrorKind::MissingHeadTable,
                 "strict integrity must require head");

    Bytes short_head = make_valid_single_font();
    put_u32(&short_head, 40U, 8U);
    ok &= expect(open_view(short_head, &view),
                 "short-head fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind == SfntIntegrityErrorKind::InvalidHeadTable,
                 "head shorter than checkSumAdjustment must fail");

    Bytes permissive = make_valid_single_font();
    permissive[69U] = std::byte{7U};
    permissive[64U] ^= std::byte{1U};
    ok &= expect(open_view(permissive, &view),
                 "permissive policy fixture must remain container-valid");
    SfntIntegrityOptions options;
    options.require_zero_padding = false;
    options.verify_table_checksums = false;
    options.verify_single_font_checksum_adjustment = false;
    SfntIntegrityStats stats;
    ok &= expect(verify_sfnt_integrity(view, options, &stats, &error),
                 "disabled optional checks must allow a policy-only verification");
    ok &= expect(stats.checksums_verified == 0U &&
                     !stats.whole_font_checksum_verified,
                 "disabled checksum policy must be reflected in statistics");
    return ok;
}

bool invalid_view_resets_outputs() {
    SfntIntegrityStats stats;
    stats.tables_seen = 99U;
    SfntIntegrityError error;
    error.kind = SfntIntegrityErrorKind::NonZeroPadding;
    const SfntResourceView invalid;
    bool ok = expect(!verify_sfnt_integrity(invalid, {}, &stats, &error),
                     "invalid view must fail integrity verification");
    ok &= expect(error.kind == SfntIntegrityErrorKind::InvalidView,
                 "invalid view must report the dedicated error");
    ok &= expect(stats.tables_seen == 0U && stats.payload_bytes_checked == 0U,
                 "failed verification must reset statistics");
    ok &= expect(std::string_view(sfnt_integrity_error_kind_name(error.kind)) ==
                     "invalid_view",
                 "integrity error names must be stable");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= valid_single_font_integrity();
    ok &= valid_collection_ignores_whole_adjustment();
    ok &= alignment_and_padding_failures();
    ok &= checksum_failures();
    ok &= head_and_policy_behavior();
    ok &= invalid_view_resets_outputs();
    if (!ok) {
        return 1;
    }
    std::cout << "font-resource-integrity-tests: PASS\n";
    return 0;
}
