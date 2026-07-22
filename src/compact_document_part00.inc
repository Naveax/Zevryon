#include "compact_document.hpp"

#include <algorithm>
#include <array>
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

constexpr std::array<char, 8> kArenaMagic{'Z', 'V', 'A', 'R', 'N', '0', '0', '1'};
constexpr std::uint32_t kArenaVersion = 1U;
constexpr std::size_t kArenaHeaderBytes = 72U;
constexpr std::size_t kRecordDescriptorBytes = 32U;
constexpr std::size_t kHeightEntryBytes = 4U;
constexpr std::size_t kBlockEntryBytes = 8U;
constexpr std::uint32_t kMaxHeightQ8 = std::numeric_limits<std::uint32_t>::max();

struct RecordDescriptorLite {
    std::uint64_t logical_id{0};
    std::uint64_t length{0};
};

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t shift = 0; shift < sizeof(T); ++shift) {
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
    for (std::size_t shift = 0; shift < sizeof(T); ++shift) {
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

std::uint64_t directory_size(const std::filesystem::path& root) {
    std::error_code error;
    std::uint64_t total = 0;
    for (std::filesystem::recursive_directory_iterator iterator(root, error), end; !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->is_regular_file(error)) {
            total += iterator->file_size(error);
        }
    }
    return total;
}

std::optional<RecordDescriptorLite> parse_record_descriptor(std::span<const std::byte> bytes) {
    if (bytes.size() != kRecordDescriptorBytes) {
        return std::nullopt;
    }
    const auto logical_id = read_le<std::uint64_t>(bytes, 0U);
    const auto length = read_le<std::uint64_t>(bytes, 16U);
    if (!logical_id || !length) {
        return std::nullopt;
    }
    return RecordDescriptorLite{*logical_id, *length};
}

std::uint32_t estimate_height_q8(std::uint64_t source_bytes, const ArenaConfig& config) {
    const std::uint64_t bytes_per_line = std::max<std::uint64_t>(1U, config.estimated_bytes_per_line);
    const std::uint64_t lines = std::max<std::uint64_t>(1U, (source_bytes + bytes_per_line - 1U) / bytes_per_line);
    const std::uint64_t line_height = std::max<std::uint64_t>(256U, config.line_height_q8);
    const std::uint64_t estimated = lines * line_height + config.vertical_padding_q8;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(estimated, kMaxHeightQ8));
}

std::vector<std::byte> serialize_header(const ArenaStats& stats) {
    std::vector<std::byte> bytes;
    bytes.reserve(kArenaHeaderBytes);
    for (const char value : kArenaMagic) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    append_le(bytes, kArenaVersion);
    append_le(bytes, stats.config.records_per_block);
    append_le(bytes, stats.config.estimated_bytes_per_line);
    append_le(bytes, stats.config.line_height_q8);
    append_le(bytes, stats.config.vertical_padding_q8);
    append_le(bytes, std::uint32_t{0U});
    append_le(bytes, stats.logical_records);
    append_le(bytes, stats.logical_nodes);
    append_le(bytes, stats.total_height_q8);
    append_le(bytes, stats.block_count);
    append_le(bytes, stats.physical_bytes);
    return bytes;
}

bool parse_header(std::span<const std::byte> bytes, ArenaStats* stats) {
    if (bytes.size() != kArenaHeaderBytes) {
        return false;
    }
    for (std::size_t index = 0; index < kArenaMagic.size(); ++index) {
        if (std::to_integer<unsigned char>(bytes[index]) != static_cast<unsigned char>(kArenaMagic[index])) {
            return false;
        }
    }
    const auto version = read_le<std::uint32_t>(bytes, 8U);
    const auto records_per_block = read_le<std::uint32_t>(bytes, 12U);
    const auto bytes_per_line = read_le<std::uint32_t>(bytes, 16U);
    const auto line_height_q8 = read_le<std::uint32_t>(bytes, 20U);
    const auto padding_q8 = read_le<std::uint32_t>(bytes, 24U);
    const auto records = read_le<std::uint64_t>(bytes, 32U);
    const auto nodes = read_le<std::uint64_t>(bytes, 40U);
    const auto total_height = read_le<std::uint64_t>(bytes, 48U);
    const auto blocks = read_le<std::uint64_t>(bytes, 56U);
    const auto physical = read_le<std::uint64_t>(bytes, 64U);
    if (!version || *version != kArenaVersion || !records_per_block || *records_per_block == 0U || !bytes_per_line ||
        *bytes_per_line == 0U || !line_height_q8 || !padding_q8 || !records || !nodes || !total_height || !blocks ||
        !physical) {
        return false;
    }
    stats->config.records_per_block = *records_per_block;
    stats->config.estimated_bytes_per_line = *bytes_per_line;
    stats->config.line_height_q8 = *line_height_q8;
    stats->config.vertical_padding_q8 = *padding_q8;
    stats->logical_records = *records;
    stats->logical_nodes = *nodes;
    stats->total_height_q8 = *total_height;
    stats->block_count = *blocks;
    stats->physical_bytes = *physical;
    return true;
}

bool read_record_descriptors(
    const std::filesystem::path& records_path,
    std::uint64_t first_record,
    std::size_t count,
    std::vector<RecordDescriptorLite>* descriptors,
    std::string* error) {
    descriptors->clear();
