#include "store_record_length_probe.hpp"

#include "massivedoc_positional_io.hpp"

#include <array>
#include <cstddef>
#include <limits>

namespace zevryon::massivedoc {
namespace {

// Store v1 record descriptors are 32 bytes; the canonical length field is the
// little-endian uint64 at byte offset 16. This probe intentionally reads only
// that 8-byte field on a worker thread instead of materializing a full reader.
constexpr std::uint64_t kRecordDescriptorBytes = 32U;
constexpr std::uint64_t kRecordLengthOffset = 16U;
constexpr std::size_t kRecordLengthBytes = 8U;

std::uint64_t decode_u64_le(
    const std::array<std::byte, kRecordLengthBytes>& bytes) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned int>(bytes[index]))
                 << (index * 8U);
    }
    return value;
}

} // namespace

bool probe_store_record_length(
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    std::uint64_t* record_length,
    std::string* error) {
    if (record_length == nullptr || error == nullptr) {
        return false;
    }
    *record_length = 0U;
    error->clear();

    BoundedPositionalReader records(
        store_root / "records.idx",
        kRecordLengthBytes);
    if (!records.open(error)) {
        return false;
    }
    if (records.file_size() % kRecordDescriptorBytes != 0U) {
        *error = "record descriptor index size is not aligned";
        return false;
    }
    const std::uint64_t record_count =
        records.file_size() / kRecordDescriptorBytes;
    if (record_index >= record_count) {
        *error = "record length probe index out of range";
        return false;
    }
    if (record_index >
        (std::numeric_limits<std::uint64_t>::max() - kRecordLengthOffset) /
            kRecordDescriptorBytes) {
        *error = "record length probe offset overflow";
        return false;
    }
    const std::uint64_t offset =
        record_index * kRecordDescriptorBytes + kRecordLengthOffset;
    std::array<std::byte, kRecordLengthBytes> bytes{};
    if (!records.read_exact_at(offset, bytes, error)) {
        return false;
    }
    *record_length = decode_u64_le(bytes);
    return true;
}

} // namespace zevryon::massivedoc
