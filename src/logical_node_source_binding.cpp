#include "logical_node_source.hpp"

#include "font_content_identity.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::size_t kStoreRecordDescriptorBytes = 32U;
constexpr std::size_t kRecordLogicalIdOffset = 0U;
constexpr std::size_t kRecordLengthOffset = 16U;
constexpr std::size_t kRecordCrcOffset = 28U;
constexpr std::string_view kRecordSequenceDomain =
    "ZEVRYON-ZVNSRC-RECORD-SEQUENCE-V1";

bool fail_binding(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool parse_sha256_hex(
    std::string_view hex,
    std::array<std::uint8_t, 32>* digest,
    std::string* error) {
    if (digest == nullptr || hex.size() != 64U) {
        return fail_binding(error, "store payload SHA-256 is malformed");
    }
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0U; index < digest->size(); ++index) {
        const int high = nibble(hex[index * 2U]);
        const int low = nibble(hex[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return fail_binding(error, "store payload SHA-256 is malformed");
        }
        (*digest)[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

template <typename T>
void append_le(std::vector<std::byte>* output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        output->push_back(static_cast<std::byte>(
            (value >> (index * 8U)) & static_cast<T>(0xffU)));
    }
}

bool update_sha(
    zevryon::text::Sha256* sha,
    std::span<const std::byte> bytes,
    std::string* error) {
    if (!sha->update(bytes)) {
        return fail_binding(error, "record-sequence SHA-256 input exceeds supported length");
    }
    return true;
}

bool compute_record_sequence_sha256(
    const std::filesystem::path& store_root,
    std::uint64_t record_count,
    std::array<std::uint8_t, 32>* digest,
    std::string* error) {
    if (digest == nullptr || error == nullptr) {
        return false;
    }
    if (record_count >
        std::numeric_limits<std::uint64_t>::max() / kStoreRecordDescriptorBytes) {
        return fail_binding(error, "record-sequence index size overflows 64-bit range");
    }

    BoundedPositionalReader records(
        store_root / "records.idx",
        kIoWindowBytes);
    if (!records.open(error)) {
        *error = "cannot open records.idx for record-sequence binding: " + *error;
        return false;
    }
    const std::uint64_t expected_size =
        record_count * static_cast<std::uint64_t>(kStoreRecordDescriptorBytes);
    if (records.file_size() != expected_size) {
        return fail_binding(error, "records.idx size changed after authoritative store open");
    }

    zevryon::text::Sha256 sha;
    const auto domain = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(kRecordSequenceDomain.data()),
        kRecordSequenceDomain.size());
    if (!update_sha(&sha, domain, error)) {
        return false;
    }
    std::vector<std::byte> prefix;
    prefix.reserve(sizeof(std::uint64_t));
    append_le(&prefix, record_count);
    if (!update_sha(&sha, prefix, error)) {
        return false;
    }

    const std::size_t records_per_batch = std::max<std::size_t>(
        1U,
        kIoWindowBytes / kStoreRecordDescriptorBytes);
    std::vector<std::byte> raw;
    std::vector<std::byte> canonical;
    raw.reserve(records_per_batch * kStoreRecordDescriptorBytes);
    canonical.reserve(records_per_batch * 28U);

    std::uint64_t first_record = 0U;
    while (first_record < record_count) {
        const std::uint64_t remaining = record_count - first_record;
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, records_per_batch));
        const std::size_t raw_bytes = count * kStoreRecordDescriptorBytes;
        raw.resize(raw_bytes);
        if (!records.read_exact_at(
                first_record * static_cast<std::uint64_t>(kStoreRecordDescriptorBytes),
                raw,
                error)) {
            *error = "cannot read records.idx for record-sequence binding: " + *error;
            return false;
        }

        canonical.clear();
        for (std::size_t relative = 0U; relative < count; ++relative) {
            const std::size_t base = relative * kStoreRecordDescriptorBytes;
            append_le(
                &canonical,
                first_record + static_cast<std::uint64_t>(relative));
            canonical.insert(
                canonical.end(),
                raw.begin() + static_cast<std::ptrdiff_t>(base + kRecordLogicalIdOffset),
                raw.begin() + static_cast<std::ptrdiff_t>(base + kRecordLogicalIdOffset + 8U));
            canonical.insert(
                canonical.end(),
                raw.begin() + static_cast<std::ptrdiff_t>(base + kRecordLengthOffset),
                raw.begin() + static_cast<std::ptrdiff_t>(base + kRecordLengthOffset + 8U));
            canonical.insert(
                canonical.end(),
                raw.begin() + static_cast<std::ptrdiff_t>(base + kRecordCrcOffset),
                raw.begin() + static_cast<std::ptrdiff_t>(base + kRecordCrcOffset + 4U));
        }
        if (!update_sha(&sha, canonical, error)) {
            return false;
        }
        first_record += static_cast<std::uint64_t>(count);
    }

    zevryon::text::Sha256Digest result{};
    if (!sha.finish(&result)) {
        return fail_binding(error, "cannot finalize record-sequence SHA-256");
    }
    for (std::size_t index = 0U; index < digest->size(); ++index) {
        (*digest)[index] = std::to_integer<std::uint8_t>(result[index]);
    }
    return true;
}

} // namespace

bool inspect_logical_node_source_store_binding(
    const std::filesystem::path& store_root,
    LogicalNodeSourceStoreBinding* binding,
    std::string* error) {
    if (binding == nullptr || error == nullptr) {
        return false;
    }
    error->clear();

    StoreReader store(store_root);
    if (!store.open(error)) {
        return false;
    }

    LogicalNodeSourceStoreBinding result;
    result.source_record_count = store.stats().corpus.logical_records;
    if (!parse_sha256_hex(store.stats().payload_sha256, &result.payload_sha256, error) ||
        !compute_record_sequence_sha256(
            store_root,
            result.source_record_count,
            &result.record_sequence_sha256,
            error)) {
        return false;
    }
    *binding = result;
    return true;
}

} // namespace zevryon::massivedoc
