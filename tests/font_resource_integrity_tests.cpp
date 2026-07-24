#include "font_resource_integrity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
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

std::span<const std::byte> slice(
    const Bytes& bytes,
    std::size_t offset,
    std::size_t length) {
    return std::span<const std::byte>(bytes.data() + offset, length);
}

Bytes make_single_font() {
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

    put_record(&bytes, 12U, sfnt_tag('c', 'm', 'a', 'p'),
               independent_checksum(slice(bytes, kCmapOffset, 5U)),
               static_cast<std::uint32_t>(kCmapOffset), 5U);
    put_record(&bytes, 28U, sfnt_tag('h', 'e', 'a', 'd'),
               independent_checksum(slice(bytes, kHeadOffset, 12U), 8U, 4U),
               static_cast<std::uint32_t>(kHeadOffset), 12U);
    put_record(&bytes, 44U, sfnt_tag('m', 'a', 'x', 'p'),
               independent_checksum(slice(bytes, kMaxpOffset, 6U)),
               static_cast<std::uint32_t>(kMaxpOffset), 6U);

    put_u32(&bytes, kHeadOffset + 8U,
            kSfntWholeFontChecksum - independent_checksum(bytes));
    return bytes;
}

Bytes make_collection() {
    constexpr std::size_t kDirectoryOffset = 16U;
    constexpr std::size_t kCmapOffset = 64U;
    constexpr std::size_t kHeadOffset = 68U;
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

    put_u32(&bytes, kCmapOffset, 0x12345678U);
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[kHeadOffset + index] =
            std::byte{static_cast<unsigned char>(0x40U + index)};
    }
    put_u32(&bytes, kHeadOffset + 8U, 0xDEADBEEFU);

    put_record(&bytes, kDirectoryOffset + 12U,
               sfnt_tag('c', 'm', 'a', 'p'),
               independent_checksum(slice(bytes, kCmapOffset, 4U)),
               static_cast<std::uint32_t>(kCmapOffset), 4U);
    put_record(&bytes, kDirectoryOffset + 28U,
               sfnt_tag('h', 'e', 'a', 'd'),
               independent_checksum(slice(bytes, kHeadOffset, 12U), 8U, 4U),
               static_cast<std::uint32_t>(kHeadOffset), 12U);
    return bytes;
}

bool open_view(const Bytes& bytes, SfntResourceView* view) {
    return open_sfnt_resource(bytes, 0U, view, nullptr, nullptr);
}

bool valid_paths() {
    Bytes single = make_single_font();
    SfntResourceView view;
    bool ok = expect(open_view(single, &view),
                     "single-font integrity fixture must parse");
    SfntIntegrityStats stats;
    ok &= expect(verify_sfnt_integrity(view, {}, &stats, nullptr),
                 "valid single font must pass strict integrity");
    ok &= expect(stats.tables_seen == 3U && stats.aligned_tables == 3U &&
                     stats.checksums_verified == 3U,
                 "single-font table statistics must be exact");
    ok &= expect(stats.payload_bytes_checked == 23U &&
                     stats.padding_bytes_checked == 5U,
                 "single-font payload and padding statistics must be exact");
    ok &= expect(stats.whole_font_checksum_verified &&
                     !stats.whole_font_checksum_ignored_for_collection,
                 "single-font whole checksum must be verified");

    Bytes collection = make_collection();
    ok &= expect(open_view(collection, &view), "TTC integrity fixture must parse");
    stats = {};
    ok &= expect(verify_sfnt_integrity(view, {}, &stats, nullptr),
                 "valid TTC must pass table integrity");
    ok &= expect(stats.checksums_verified == 2U &&
                     !stats.whole_font_checksum_verified &&
                     stats.whole_font_checksum_ignored_for_collection,
                 "TTC must verify table checksums and ignore whole adjustment");

    const std::array<std::byte, 3U> partial{
        std::byte{0x12U}, std::byte{0x34U}, std::byte{0x56U}};
    ok &= expect(calculate_sfnt_checksum(partial) == 0x12345600U,
                 "checksum must zero-pad a partial final word");
    return ok;
}

bool alignment_and_padding_failures() {
    SfntResourceView view;
    SfntIntegrityError error;
    bool ok = true;

    Bytes misaligned = make_single_font();
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
                 "misaligned table must fail strict integrity");

    Bytes non_zero = make_single_font();
    non_zero[69U] = std::byte{1U};
    ok &= expect(open_view(non_zero, &view),
                 "non-zero padding fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind == SfntIntegrityErrorKind::NonZeroPadding &&
                     error.byte_offset == 69U,
                 "non-zero padding must report its exact byte");

    Bytes missing = make_single_font();
    missing.resize(90U);
    ok &= expect(open_view(missing, &view),
                 "missing padding fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind == SfntIntegrityErrorKind::PaddingOutOfBounds,
                 "missing terminal padding must fail integrity");
    return ok;
}

bool checksum_and_head_failures() {
    SfntResourceView view;
    SfntIntegrityError error;
    bool ok = true;

    Bytes table_corruption = make_single_font();
    table_corruption[64U] ^= std::byte{1U};
    ok &= expect(open_view(table_corruption, &view),
                 "table-corruption fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind == SfntIntegrityErrorKind::TableChecksumMismatch &&
                     error.table_tag == sfnt_tag('c', 'm', 'a', 'p'),
                 "table corruption must fail its directory checksum");

    Bytes adjustment_corruption = make_single_font();
    adjustment_corruption[80U] ^= std::byte{1U};
    ok &= expect(open_view(adjustment_corruption, &view),
                 "adjustment-corruption fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind ==
                         SfntIntegrityErrorKind::WholeFontChecksumMismatch,
                 "checkSumAdjustment corruption must fail the whole checksum");

    Bytes missing_head = make_single_font();
    put_u32(&missing_head, 28U, sfnt_tag('k', 'e', 'r', 'n'));
    ok &= expect(open_view(missing_head, &view),
                 "missing-head fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind == SfntIntegrityErrorKind::MissingHeadTable,
                 "strict policy must require head");

    Bytes short_head = make_single_font();
    put_u32(&short_head, 40U, 8U);
    ok &= expect(open_view(short_head, &view),
                 "short-head fixture must remain container-valid");
    ok &= expect(!verify_sfnt_integrity(view, {}, nullptr, &error) &&
                     error.kind == SfntIntegrityErrorKind::InvalidHeadTable,
                 "short head must fail before checksum access");
    return ok;
}

bool policy_and_atomic_outputs() {
    Bytes bytes = make_single_font();
    bytes[64U] ^= std::byte{1U};
    bytes[69U] = std::byte{7U};
    SfntResourceView view;
    bool ok = expect(open_view(bytes, &view),
                     "policy fixture must remain container-valid");
    SfntIntegrityOptions options;
    options.require_zero_padding = false;
    options.verify_table_checksums = false;
    options.verify_single_font_checksum_adjustment = false;
    SfntIntegrityStats stats;
    SfntIntegrityError error;
    ok &= expect(verify_sfnt_integrity(view, options, &stats, &error),
                 "disabled optional checks must permit policy-only verification");
    ok &= expect(stats.checksums_verified == 0U &&
                     !stats.whole_font_checksum_verified,
                 "disabled checks must be reflected in statistics");

    stats.tables_seen = 99U;
    error.kind = SfntIntegrityErrorKind::NonZeroPadding;
    const SfntResourceView invalid;
    ok &= expect(!verify_sfnt_integrity(invalid, {}, &stats, &error),
                 "invalid view must fail");
    ok &= expect(error.kind == SfntIntegrityErrorKind::InvalidView &&
                     stats.tables_seen == 0U,
                 "failure must reset outputs before reporting invalid view");
    ok &= expect(std::string_view(sfnt_integrity_error_kind_name(error.kind)) ==
                     "invalid_view",
                 "integrity error names must remain stable");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= valid_paths();
    ok &= alignment_and_padding_failures();
    ok &= checksum_and_head_failures();
    ok &= policy_and_atomic_outputs();
    if (!ok) {
        return 1;
    }
    std::cout << "font-resource-integrity-tests: PASS\n";
    return 0;
}
