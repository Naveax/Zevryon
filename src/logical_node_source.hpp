#pragma once

#include "logical_node_arena.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace zevryon::massivedoc {

inline constexpr std::uint32_t kLogicalNodeSourceFormatV1 = 1U;
inline constexpr std::uint32_t kLogicalNodeSourceFormatV2 = 2U;
inline constexpr std::uint32_t kLogicalNodeSourceCurrentFormat =
    kLogicalNodeSourceFormatV2;

struct LogicalNodeSourceStoreBinding {
    std::uint64_t source_record_count{0U};
    std::array<std::uint8_t, 32> payload_sha256{};
    std::array<std::uint8_t, 32> record_sequence_sha256{};
};

struct LogicalNodeSourceManifest {
    std::uint32_t format_version{0U};
    std::uint64_t node_count{0U};
    std::uint64_t attribute_count{0U};
    LogicalNodeSourceStoreBinding store_binding{};
};

struct LogicalNodeSourceAttribute {
    std::string name;
    std::string value;
    std::uint32_t flags{0U};
};

struct LogicalNodeSourceNode {
    std::uint64_t logical_id{0U};
    std::uint64_t source_record_index{0U};
    std::uint64_t source_byte_offset{0U};
    // V1: length must remain inside source_record_index.
    // V2: total contiguous logical byte length starting at the record-local
    // position; the span may continue through following physical records.
    std::uint64_t source_byte_length{0U};
    std::uint64_t parent_ordinal{kNoLogicalNodeOrdinal};
    std::string tag;
    std::string role;
    std::string style;
    std::uint32_t flags{0U};
    std::vector<LogicalNodeSourceAttribute> attributes;
};

class LogicalNodeSourceWriter {
public:
    explicit LogicalNodeSourceWriter(
        std::filesystem::path output_path,
        std::uint32_t format_version = kLogicalNodeSourceCurrentFormat);
    ~LogicalNodeSourceWriter();

    LogicalNodeSourceWriter(const LogicalNodeSourceWriter&) = delete;
    LogicalNodeSourceWriter& operator=(const LogicalNodeSourceWriter&) = delete;
    LogicalNodeSourceWriter(LogicalNodeSourceWriter&&) noexcept;
    LogicalNodeSourceWriter& operator=(LogicalNodeSourceWriter&&) noexcept;

    bool begin(std::string* error);
    bool append_node(
        const LogicalNodeInput& node,
        std::span<const LogicalNodeAttributeInput> attributes,
        std::string* error);
    bool finish(const LogicalNodeSourceStoreBinding& store_binding, std::string* error);

    std::uint64_t node_count() const noexcept;
    std::uint64_t attribute_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class LogicalNodeSourceReader {
public:
    explicit LogicalNodeSourceReader(std::filesystem::path source_path);
    ~LogicalNodeSourceReader();

    LogicalNodeSourceReader(const LogicalNodeSourceReader&) = delete;
    LogicalNodeSourceReader& operator=(const LogicalNodeSourceReader&) = delete;
    LogicalNodeSourceReader(LogicalNodeSourceReader&&) noexcept;
    LogicalNodeSourceReader& operator=(LogicalNodeSourceReader&&) noexcept;

    bool open(std::string* error);
    const LogicalNodeSourceManifest& manifest() const noexcept;

    // On success, has_node=false means the manifest-declared stream was consumed
    // completely and no trailing bytes were present.
    bool next(LogicalNodeSourceNode* node, bool* has_node, std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct LogicalNodeSourceImportConfig {
    std::string candidate_commit;
    std::string candidate_tree;
    std::uint32_t semantic_bucket_count{4096U};
    std::uint32_t semantic_hash_bits{64U};
};

// Computes the exact immutable store binding used by ZVNSRC01. The payload
// digest binds logical bytes while record_sequence_sha256 binds stable physical
// record order/boundaries through ordinal + record logical id + length + CRC32.
// Chunk placement is deliberately excluded so storage compaction does not
// invalidate semantic source identity.
bool inspect_logical_node_source_store_binding(
    const std::filesystem::path& store_root,
    LogicalNodeSourceStoreBinding* binding,
    std::string* error);

// Imports an explicit parser/import-produced semantic node stream into the
// disk-backed arena. The node source must bind both the exact StoreReader
// payload SHA-256 and the exact stable physical-record sequence. V1 source
// ranges are validated inside one physical record. V2 source spans are
// validated as one contiguous logical byte span that may cross record
// boundaries. The store manifest's logical_nodes field is only a count
// consistency check and is never used to synthesize semantic nodes.
bool import_logical_node_source_to_arena(
    const std::filesystem::path& source_path,
    const std::filesystem::path& store_root,
    LogicalNodeSourceImportConfig config,
    std::string* error);

} // namespace zevryon::massivedoc
