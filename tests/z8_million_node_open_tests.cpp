#include "logical_node_arena.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::massivedoc::LogicalNodeArenaReader;
using zevryon::massivedoc::LogicalNodeRecord;
using zevryon::massivedoc::LogicalSemanticKind;
using zevryon::massivedoc::kNoLogicalNodeOrdinal;

constexpr std::uint64_t kNodeCount = 1'000'000U;
constexpr std::uint64_t kNodeRecordBytes = 96U;
constexpr std::uint64_t kAttributeRecordBytes = 16U;
constexpr std::uint64_t kColdBytesPerNodeLimit = 97U;
constexpr double kOpenLatencyLimitMs = 2'000.0;
constexpr double kTailLookupLatencyLimitMs = 2'000.0;
constexpr std::array<std::uint8_t, 8> kManifestMagic{
    'Z', 'V', 'N', 'O', 'D', 'A', '0', '1'};

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: z8 million-node open: " << message << '\n';
        return false;
    }
    return true;
}

struct Cleanup final {
    explicit Cleanup(std::filesystem::path value) : root(std::move(value)) {}
    ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    Cleanup(const Cleanup&) = delete;
    Cleanup& operator=(const Cleanup&) = delete;
    std::filesystem::path root;
};

std::filesystem::path unique_root() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string("zevryon-z8-million-open-") + std::to_string(tick));
}

void append_u32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        bytes->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        bytes->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_string(std::vector<std::uint8_t>* bytes, std::string_view value) {
    for (const char character : value) {
        bytes->push_back(static_cast<std::uint8_t>(
            static_cast<unsigned char>(character)));
    }
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= static_cast<std::uint32_t>(byte);
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

std::uint64_t semantic_hash(std::string_view value) {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const char character : value) {
        hash ^= static_cast<std::uint64_t>(
            static_cast<unsigned char>(character));
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

bool write_bytes(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    if (!bytes.empty()) {
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    stream.flush();
    return static_cast<bool>(stream);
}

bool touch(const std::filesystem::path& path) {
    const std::array<std::uint8_t, 0> empty{};
    return write_bytes(path, empty);
}

std::vector<std::uint8_t> encode_node(
    std::uint64_t logical_id,
    std::uint64_t parent_ordinal) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(kNodeRecordBytes));
    append_u64(&bytes, logical_id);
    append_u64(&bytes, 0U); // source record index
    append_u64(&bytes, 0U); // source byte offset
    append_u64(&bytes, 0U); // source byte length
    append_u64(&bytes, parent_ordinal);
    append_u64(&bytes, kNoLogicalNodeOrdinal);
    append_u64(&bytes, kNoLogicalNodeOrdinal);
    append_u64(&bytes, 0U); // attribute offset
    append_u32(&bytes, 0U); // attribute count
    append_u32(&bytes, 1U); // tag id
    append_u32(&bytes, 0U); // role id
    append_u32(&bytes, 0U); // style id
    append_u32(&bytes, 0U); // flags
    append_u32(&bytes, 0U); // reserved
    append_u32(&bytes, 0U); // reserved
    append_u32(
        &bytes,
        crc32(std::span<const std::uint8_t>(bytes.data(), bytes.size())));
    return bytes;
}

std::vector<std::uint8_t> encode_tag_dictionary() {
    constexpr std::string_view value = "#document";
    std::vector<std::uint8_t> prefix;
    prefix.reserve(28U);
    append_u32(&prefix, 1U);
    append_u32(&prefix, 0U);
    append_u64(&prefix, semantic_hash(value));
    append_u64(&prefix, 0U);
    append_u32(&prefix, static_cast<std::uint32_t>(value.size()));

    std::vector<std::uint8_t> crc_material = prefix;
    append_string(&crc_material, value);

    std::vector<std::uint8_t> output = prefix;
    append_u32(
        &output,
        crc32(std::span<const std::uint8_t>(
            crc_material.data(), crc_material.size())));
    append_string(&output, value);
    return output;
}

std::vector<std::uint8_t> encode_manifest() {
    constexpr std::string_view commit =
        "0123456789abcdef0123456789abcdef01234567";
    constexpr std::string_view tree =
        "89abcdef0123456789abcdef0123456789abcdef";
    static_assert(commit.size() == 40U);
    static_assert(tree.size() == 40U);

    std::vector<std::uint8_t> bytes;
    bytes.reserve(188U);
    bytes.insert(bytes.end(), kManifestMagic.begin(), kManifestMagic.end());
    append_u32(&bytes, 1U);   // format version
    append_u32(&bytes, 64U);  // semantic bucket count
    append_u32(&bytes, 64U);  // semantic hash bits
    append_u32(&bytes, 0U);   // reserved
    append_u64(&bytes, kNodeCount);
    append_u64(&bytes, 0U);   // attributes
    append_u32(&bytes, 1U);   // tag semantics
    append_u32(&bytes, 0U);   // role semantics
    append_u32(&bytes, 0U);   // style semantics
    append_u32(&bytes, 0U);   // attribute-name semantics
    append_u32(&bytes, 0U);   // attribute-value semantics
    append_u32(&bytes, 0U);   // reserved
    append_string(&bytes, commit);
    append_string(&bytes, tree);
    for (std::uint32_t index = 0U; index < 32U; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(index));
    }
    append_u32(&bytes, static_cast<std::uint32_t>(kNodeRecordBytes));
    append_u32(&bytes, static_cast<std::uint32_t>(kAttributeRecordBytes));
    append_u32(
        &bytes,
        crc32(std::span<const std::uint8_t>(bytes.data(), bytes.size())));
    return bytes;
}

bool create_sparse_nodes(const std::filesystem::path& path) {
    const std::vector<std::uint8_t> root =
        encode_node(1U, kNoLogicalNodeOrdinal);
    const std::vector<std::uint8_t> tail =
        encode_node(kNodeCount, 0U);
    if (root.size() != kNodeRecordBytes || tail.size() != kNodeRecordBytes) {
        return false;
    }

    std::fstream stream(
        path,
        std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(
        reinterpret_cast<const char*>(root.data()),
        static_cast<std::streamsize>(root.size()));
    const std::uint64_t tail_offset = (kNodeCount - 1U) * kNodeRecordBytes;
    stream.seekp(static_cast<std::streamoff>(tail_offset), std::ios::beg);
    stream.write(
        reinterpret_cast<const char*>(tail.data()),
        static_cast<std::streamsize>(tail.size()));
    stream.flush();
    return static_cast<bool>(stream);
}

bool create_fixture(const std::filesystem::path& root) {
    const std::filesystem::path arena = root / "node-arena";
    std::error_code error;
    std::filesystem::create_directories(arena, error);
    if (error) {
        return false;
    }

    const std::vector<std::uint8_t> manifest = encode_manifest();
    if (manifest.size() != 188U ||
        !write_bytes(arena / "manifest.bin", manifest) ||
        !create_sparse_nodes(arena / "nodes.bin") ||
        !touch(arena / "attributes.bin")) {
        return false;
    }

    const std::vector<std::uint8_t> tag_entries = encode_tag_dictionary();
    std::vector<std::uint8_t> tag_offsets;
    append_u64(&tag_offsets, 1U);
    if (!write_bytes(arena / "tag.entries", tag_entries) ||
        !write_bytes(arena / "tag.offsets", tag_offsets)) {
        return false;
    }

    constexpr std::array<std::string_view, 4> empty_prefixes{
        "role", "style", "attribute-name", "attribute-value"};
    for (const std::string_view prefix : empty_prefixes) {
        if (!touch(arena / (std::string(prefix) + ".entries")) ||
            !touch(arena / (std::string(prefix) + ".offsets"))) {
            return false;
        }
    }
    return true;
}

std::uintmax_t logical_sidecar_bytes(const std::filesystem::path& arena) {
    std::uintmax_t total = 0U;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(arena, error)) {
        if (error) {
            return 0U;
        }
        if (entry.is_regular_file(error)) {
            if (error) {
                return 0U;
            }
            total += entry.file_size(error);
            if (error) {
                return 0U;
            }
        }
    }
    return error ? 0U : total;
}

} // namespace

int main() {
    const std::filesystem::path root = unique_root();
    Cleanup cleanup(root);
    if (!require(create_fixture(root), "synthetic cold arena fixture creation")) {
        return 1;
    }

    const std::uintmax_t sidecar_bytes =
        logical_sidecar_bytes(root / "node-arena");
    const std::uintmax_t cold_budget =
        static_cast<std::uintmax_t>(kNodeCount * kColdBytesPerNodeLimit);
    if (!require(sidecar_bytes > 0U, "logical sidecar bytes are measurable") ||
        !require(sidecar_bytes <= cold_budget,
                 "cold logical-node sidecar stays within 97 bytes per node")) {
        return 1;
    }

    LogicalNodeArenaReader reader(root);
    std::string error;
    const auto open_begin = std::chrono::steady_clock::now();
    const bool opened = reader.open(&error);
    const auto open_end = std::chrono::steady_clock::now();
    const double open_ms =
        std::chrono::duration<double, std::milli>(open_end - open_begin).count();
    if (!require(opened, error) ||
        !require(reader.manifest().node_count == kNodeCount,
                 "million-node manifest opens without whole-arena materialization") ||
        !require(open_ms <= kOpenLatencyLimitMs,
                 "million-node cold open remains bounded")) {
        return 1;
    }

    LogicalNodeRecord tail;
    const auto tail_begin = std::chrono::steady_clock::now();
    const bool tail_ok = reader.node_by_id(kNodeCount, &tail, &error);
    const auto tail_end = std::chrono::steady_clock::now();
    const double tail_ms =
        std::chrono::duration<double, std::milli>(tail_end - tail_begin).count();
    if (!require(tail_ok, error) ||
        !require(tail.logical_id == kNodeCount && tail.parent_ordinal == 0U,
                 "last logical node identity is stable and directly seekable") ||
        !require(tail_ms <= kTailLookupLatencyLimitMs,
                 "million-node tail lookup remains bounded")) {
        return 1;
    }

    std::string tag;
    if (!require(
            reader.resolve_semantic(LogicalSemanticKind::tag, 1U, &tag, &error),
            error) ||
        !require(tag == "#document", "cold semantic dictionary resolves exactly")) {
        return 1;
    }

    std::cout
        << "{\"schema\":\"zevryon.z8-million-node-open.v1\","
        << "\"node_count\":" << kNodeCount << ','
        << "\"sidecar_bytes\":" << sidecar_bytes << ','
        << "\"bytes_per_node\":"
        << static_cast<double>(sidecar_bytes) /
               static_cast<double>(kNodeCount)
        << ','
        << "\"open_ms\":" << open_ms << ','
        << "\"tail_lookup_ms\":" << tail_ms << ','
        << "\"passed\":true}\n";
    return 0;
}
