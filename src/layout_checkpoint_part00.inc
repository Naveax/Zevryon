#include "layout_checkpoint.hpp"

#include "massivedoc_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <span>
#include <system_error>
#include <type_traits>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::array<char, 8> kCheckpointMagic{'Z', 'V', 'C', 'K', 'P', '0', '0', '1'};
constexpr std::uint32_t kCheckpointVersion = 1U;
constexpr std::size_t kCheckpointHeaderBytes = 96U;
constexpr std::size_t kCheckpointEntryBytes = 24U;
constexpr std::size_t kRecordDescriptorBytes = 32U;
constexpr std::uint32_t kCheckpointFlagHeightSaturated = 1U;

struct RecordInfo {
    std::uint64_t logical_id{0};
    std::uint64_t source_bytes{0};
};

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t shift = 0U; shift < sizeof(T); ++shift) {
        output.push_back(static_cast<std::byte>((value >> (shift * 8U)) & static_cast<T>(0xffU)));
    }
}

template <typename T>
std::optional<T> read_le(std::span<const std::byte> input, std::size_t offset) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > input.size() || input.size() - offset < sizeof(T)) {
        return std::nullopt;
    }
    T value{0};
    for (std::size_t shift = 0U; shift < sizeof(T); ++shift) {
        value |= static_cast<T>(std::to_integer<unsigned int>(input[offset + shift])) << (shift * 8U);
    }
    return value;
}

bool write_all(std::ostream& stream, std::span<const std::byte> bytes) {
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

bool read_exact(std::istream& stream, std::span<std::byte> bytes) {
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.gcount() == static_cast<std::streamsize>(bytes.size());
}

bool seek_stream(std::istream& stream, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    return static_cast<bool>(stream);
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

std::uint64_t saturating_multiply(std::uint64_t left, std::uint64_t right) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t value = 1469598103934665603ULL;
    for (const std::byte byte : bytes) {
        value ^= static_cast<std::uint64_t>(std::to_integer<unsigned int>(byte));
        value *= 1099511628211ULL;
    }
    return value;
}

void hash_value(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (std::size_t shift = 0U; shift < sizeof(value); ++shift) {
        *hash ^= (value >> (shift * 8U)) & 0xffU;
        *hash *= 1099511628211ULL;
    }
}

std::uint64_t config_hash(const LayoutCheckpointConfig& config) noexcept {
    std::uint64_t value = 1469598103934665603ULL;
    hash_value(&value, config.width_q8);
    hash_value(&value, config.average_advance_q8);
    hash_value(&value, config.line_height_q8);
    hash_value(&value, config.horizontal_padding_q8);
    hash_value(&value, config.vertical_padding_q8);
    hash_value(&value, config.target_stride_bytes);
    return value;
}

bool valid_config(const LayoutCheckpointConfig& config) noexcept {
    return config.width_q8 != 0U && config.average_advance_q8 != 0U && config.line_height_q8 != 0U &&
           config.target_stride_bytes != 0U;
}

bool configs_equal(const LayoutCheckpointConfig& left, const LayoutCheckpointConfig& right) noexcept {
    return left.width_q8 == right.width_q8 && left.average_advance_q8 == right.average_advance_q8 &&
           left.line_height_q8 == right.line_height_q8 &&
           left.horizontal_padding_q8 == right.horizontal_padding_q8 &&
           left.vertical_padding_q8 == right.vertical_padding_q8 &&
           left.target_stride_bytes == right.target_stride_bytes;
}

bool read_record_info(
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    RecordInfo* info,
    std::string* error) {
    if (info == nullptr || error == nullptr ||
        record_index > std::numeric_limits<std::uint64_t>::max() / kRecordDescriptorBytes) {
        if (error != nullptr) {
            *error = "invalid record-info request";
        }
        return false;
    }
    std::ifstream stream(store_root / "records.idx", std::ios::binary);
    const std::uint64_t offset = record_index * kRecordDescriptorBytes;
    std::array<std::byte, kRecordDescriptorBytes> bytes{};
    if (!stream || !seek_stream(stream, offset) || !read_exact(stream, bytes)) {
        *error = "cannot read checkpoint record descriptor";
        return false;
    }
    const auto logical_id = read_le<std::uint64_t>(bytes, 0U);
    const auto source_bytes = read_le<std::uint64_t>(bytes, 16U);
    if (!logical_id || !source_bytes) {
        *error = "invalid checkpoint record descriptor";
        return false;
    }
    info->logical_id = *logical_id;
    info->source_bytes = *source_bytes;
    return true;
}

std::vector<std::byte> serialize_entries(const std::vector<LayoutCheckpointEntry>& entries) {
    std::vector<std::byte> bytes;
    if (entries.size() <= std::numeric_limits<std::size_t>::max() / kCheckpointEntryBytes) {
        bytes.reserve(entries.size() * kCheckpointEntryBytes);
    }
    for (const LayoutCheckpointEntry& entry : entries) {
        append_le(bytes, entry.source_offset);
        append_le(bytes, entry.line_index);
        append_le(bytes, entry.y_q8);
    }
    return bytes;
}

std::vector<std::byte> serialize_header(const LayoutCheckpointStats& stats, std::uint64_t checksum) {
    std::vector<std::byte> bytes;
    bytes.reserve(kCheckpointHeaderBytes);
    for (const char value : kCheckpointMagic) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    append_le(bytes, kCheckpointVersion);
    append_le(bytes, stats.height_saturated ? kCheckpointFlagHeightSaturated : std::uint32_t{0U});
    append_le(bytes, stats.record_index);
    append_le(bytes, stats.logical_id);
    append_le(bytes, stats.source_bytes);
    append_le(bytes, stats.config.width_q8);
    append_le(bytes, stats.config.average_advance_q8);
    append_le(bytes, stats.config.line_height_q8);
    append_le(bytes, stats.config.horizontal_padding_q8);
    append_le(bytes, stats.config.vertical_padding_q8);
    append_le(bytes, stats.config.target_stride_bytes);
    append_le(bytes, stats.measured_height_q8);
    append_le(bytes, std::uint32_t{0U});
    append_le(bytes, stats.entry_count);
    append_le(bytes, stats.physical_bytes);
    append_le(bytes, checksum);
