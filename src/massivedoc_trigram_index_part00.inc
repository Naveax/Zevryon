#include "massivedoc_trigram_index.hpp"
#include "massivedoc_positional_io.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

constexpr std::array<char, 8> kIndexMagic{'Z', 'M', 'D', 'T', 'R', 'I', '0', '1'};
constexpr std::array<char, 8> kBloomMagic{'Z', 'M', 'D', 'B', 'L', 'M', '0', '1'};
constexpr std::uint32_t kIndexVersion = 3U;
constexpr std::uint32_t kBloomVersion = 1U;
constexpr std::size_t kHeaderBytes = 112U;
constexpr std::size_t kBloomHeaderBytes = 80U;
constexpr std::size_t kDirectoryEntryBytes = 40U;
constexpr std::size_t kBloomFrameBytes = kTrigramBloomBytes + sizeof(std::uint32_t);
constexpr std::size_t kSpoolEntryBytes = 12U;
constexpr std::size_t kCopyWindowBytes = 64U * 1024U;
constexpr std::uint32_t kBloomBits = static_cast<std::uint32_t>(kTrigramBloomBytes * 8U);

struct SpoolEntry {
    std::uint32_t trigram{0U};
    std::uint64_t block{0U};
};

struct DirectoryEntry {
    std::uint32_t trigram{0U};
    std::uint32_t postings_crc32{0U};
    std::uint64_t postings_offset{0U};
    std::uint64_t postings_bytes{0U};
    std::uint64_t block_count{0U};
};

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>((value >> (index * 8U)) & static_cast<T>(0xffU)));
    }
}

template <typename T>
std::optional<T> read_le(std::span<const std::byte> bytes, std::size_t offset) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
        return std::nullopt;
    }
    T value = 0U;
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8U);
    }
    return value;
}

std::uint32_t crc32_update(std::uint32_t crc, std::span<const std::byte> bytes) {
    crc = ~crc;
    for (const std::byte raw : bytes) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<unsigned char>(raw));
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

bool write_all(std::ostream& stream, std::span<const std::byte> bytes) {
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

std::uint32_t make_trigram(unsigned char first, unsigned char second, unsigned char third) {
    return (static_cast<std::uint32_t>(first) << 16U) |
           (static_cast<std::uint32_t>(second) << 8U) |
           static_cast<std::uint32_t>(third);
}

std::uint32_t mix32(std::uint32_t value) {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

std::array<std::uint32_t, 3> bloom_positions(std::uint32_t trigram) {
    return {
        mix32(trigram ^ 0x243f6a88U) % kBloomBits,
        mix32(trigram ^ 0x85a308d3U) % kBloomBits,
        mix32(trigram ^ 0x13198a2eU) % kBloomBits,
    };
}

void bloom_add(std::array<std::byte, kTrigramBloomBytes>* bloom, std::uint32_t trigram) {
    for (const std::uint32_t bit : bloom_positions(trigram)) {
        const std::size_t byte_index = static_cast<std::size_t>(bit >> 3U);
        const unsigned int bit_index = bit & 7U;
        (*bloom)[byte_index] |= static_cast<std::byte>(1U << bit_index);
    }
}

bool bloom_has(std::span<const std::byte> bloom, std::uint32_t trigram) {
    for (const std::uint32_t bit : bloom_positions(trigram)) {
        const std::size_t byte_index = static_cast<std::size_t>(bit >> 3U);
        const unsigned int bit_index = bit & 7U;
        if ((std::to_integer<unsigned int>(bloom[byte_index]) & (1U << bit_index)) == 0U) {
            return false;
        }
    }
    return true;
}

std::vector<std::byte> serialize_spool(const SpoolEntry& entry) {
    std::vector<std::byte> bytes;
    bytes.reserve(kSpoolEntryBytes);
    append_le(bytes, entry.trigram);
    append_le(bytes, entry.block);
    return bytes;
}

std::optional<SpoolEntry> parse_spool(std::span<const std::byte> bytes) {
    if (bytes.size() != kSpoolEntryBytes) {
        return std::nullopt;
    }
    const auto trigram = read_le<std::uint32_t>(bytes, 0U);
    const auto block = read_le<std::uint64_t>(bytes, 4U);
    if (!trigram || !block) {
        return std::nullopt;
    }
    return SpoolEntry{*trigram, *block};
}

bool entry_less(const SpoolEntry& left, const SpoolEntry& right) {
    if (left.trigram != right.trigram) {
        return left.trigram < right.trigram;
    }
    return left.block < right.block;
}

bool entry_equal(const SpoolEntry& left, const SpoolEntry& right) {
    return left.trigram == right.trigram && left.block == right.block;
}

bool read_spool_entry(std::istream& stream, SpoolEntry* entry, bool* eof, std::string* error) {
    std::array<std::byte, kSpoolEntryBytes> bytes{};
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() == 0 && stream.eof()) {
        *eof = true;
        return true;
    }
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        *error = "truncated trigram spool/run entry";
        return false;
    }
    const auto parsed = parse_spool(bytes);
    if (!parsed) {
        *error = "invalid trigram spool/run entry";
        return false;
    }
    *entry = *parsed;
    *eof = false;
    return true;
}

bool write_spool_entry(std::ostream& stream, const SpoolEntry& entry, std::string* error) {
    const auto bytes = serialize_spool(entry);
    if (!write_all(stream, bytes)) {
        *error = "failed to write trigram run entry";
        return false;
    }
    return true;
}

bool cancellation_requested(const TrigramCancellationCheck& cancelled) {
    return static_cast<bool>(cancelled) && cancelled();
}

bool copy_file_bounded(
    const std::filesystem::path& source,
    std::ostream& destination,
    std::string* error) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        *error = "cannot open trigram temporary file for copy";
        return false;
    }
    std::array<std::byte, kCopyWindowBytes> buffer{};
    for (;;) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto got = input.gcount();
        if (got < 0) {
            *error = "invalid trigram temporary read count";
            return false;
        }
        if (got != 0) {
            if (!write_all(destination, std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(got)))) {
                *error = "failed to copy trigram temporary file";
                return false;
            }
        }
        if (input.eof()) {
            break;
