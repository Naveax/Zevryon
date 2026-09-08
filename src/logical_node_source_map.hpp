#pragma once

#include "logical_node_arena.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

struct LogicalNodeSourceMapBuildConfig {
    std::string candidate_commit;
    std::string candidate_tree;
    std::array<std::uint8_t, 32> source_sha256{};
    std::uint64_t source_record_count{0U};
    std::uint64_t node_count{0U};
};

struct LogicalNodeSourceMapManifest {
    std::uint32_t format_version{0U};
    std::uint64_t source_record_count{0U};
    std::uint64_t node_count{0U};
    std::uint64_t posting_count{0U};
    std::string candidate_commit;
    std::string candidate_tree;
    std::array<std::uint8_t, 32> source_sha256{};
};

struct LogicalNodeSourceMapCursor {
    std::uint64_t source_record_index{0U};
    std::uint64_t next_posting_plus_one{0U};
    std::uint64_t visited_postings{0U};
};

class LogicalNodeSourceMapWriter {
public:
    LogicalNodeSourceMapWriter(
        std::filesystem::path store_root,
        LogicalNodeSourceMapBuildConfig config);
    ~LogicalNodeSourceMapWriter();

    LogicalNodeSourceMapWriter(const LogicalNodeSourceMapWriter&) = delete;
    LogicalNodeSourceMapWriter& operator=(const LogicalNodeSourceMapWriter&) = delete;
    LogicalNodeSourceMapWriter(LogicalNodeSourceMapWriter&&) noexcept;
    LogicalNodeSourceMapWriter& operator=(LogicalNodeSourceMapWriter&&) noexcept;

    bool begin(std::string* error);
    bool append_binding(
        std::uint64_t source_record_index,
        std::uint64_t node_ordinal,
        std::string* error);
    bool finish(std::string* error);

    std::uint64_t posting_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class LogicalNodeSourceMapReader {
public:
    explicit LogicalNodeSourceMapReader(std::filesystem::path store_root);
    ~LogicalNodeSourceMapReader();

    LogicalNodeSourceMapReader(const LogicalNodeSourceMapReader&) = delete;
    LogicalNodeSourceMapReader& operator=(const LogicalNodeSourceMapReader&) = delete;
    LogicalNodeSourceMapReader(LogicalNodeSourceMapReader&&) noexcept;
    LogicalNodeSourceMapReader& operator=(LogicalNodeSourceMapReader&&) noexcept;

    bool open(std::string* error);
    const LogicalNodeSourceMapManifest& manifest() const noexcept;

    bool verify_node_arena_identity(
        const LogicalNodeArenaManifest& arena,
        std::string* error) const;

    bool first_node_for_record(
        std::uint64_t source_record_index,
        LogicalNodeSourceMapCursor* cursor,
        std::uint64_t* node_ordinal,
        bool* found,
        std::string* error) const;
    bool next_node_for_record(
        LogicalNodeSourceMapCursor* cursor,
        std::uint64_t* node_ordinal,
        bool* found,
        std::string* error) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc
