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

struct LogicalNodeSourceManifest {
    std::uint32_t format_version{0U};
    std::uint64_t node_count{0U};
    std::uint64_t attribute_count{0U};
    std::array<std::uint8_t, 32> source_sha256{};
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
    LogicalNodeSourceWriter(
        std::filesystem::path output_path,
        std::array<std::uint8_t, 32> source_sha256);
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
    bool finish(std::string* error);

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

// Imports an explicit parser/import-produced semantic node stream into the
// disk-backed arena. The node source must bind the exact StoreReader payload
// SHA-256 and every source range is independently checked against its record.
// The store manifest's logical_nodes field is used only as a count consistency
// check; it is never used to synthesize semantic nodes.
bool import_logical_node_source_to_arena(
    const std::filesystem::path& source_path,
    const std::filesystem::path& store_root,
    LogicalNodeSourceImportConfig config,
    std::string* error);

} // namespace zevryon::massivedoc
