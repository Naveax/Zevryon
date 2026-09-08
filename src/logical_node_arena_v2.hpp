#pragma once

#include "logical_node_arena.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace zevryon::massivedoc {

inline constexpr std::uint32_t kLogicalNodeArenaV2FormatVersion = 2U;

struct LogicalNodeArenaV2Manifest {
    std::uint32_t format_version{0U};
    LogicalNodeArenaManifest storage_manifest{};
};

class LogicalNodeArenaV2StoreBoundWriter;
class LogicalNodeArenaV2StoreBoundReader;

// Low-level v2 storage engine. Construction and I/O are intentionally private:
// authoritative production code must use the store-bound wrapper so cross-record
// source triples cannot be reopened against a different physical record sequence.
class LogicalNodeArenaV2Writer final {
public:
    ~LogicalNodeArenaV2Writer();

    LogicalNodeArenaV2Writer(const LogicalNodeArenaV2Writer&) = delete;
    LogicalNodeArenaV2Writer& operator=(const LogicalNodeArenaV2Writer&) = delete;
    LogicalNodeArenaV2Writer(LogicalNodeArenaV2Writer&&) noexcept;
    LogicalNodeArenaV2Writer& operator=(LogicalNodeArenaV2Writer&&) noexcept;

private:
    friend class LogicalNodeArenaV2StoreBoundWriter;

    LogicalNodeArenaV2Writer(
        std::filesystem::path store_root,
        LogicalNodeArenaBuildConfig config);

    bool begin(std::string* error);
    bool append_node(
        const LogicalNodeInput& node,
        std::span<const LogicalNodeAttributeInput> attributes,
        std::string* error);
    bool finish(std::string* error);

    std::uint64_t node_count() const noexcept;
    std::uint64_t attribute_count() const noexcept;
    std::uint64_t semantic_bucket_head_bytes() const noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Low-level v2 storage reader. The public store-bound reader validates the exact
// native-store payload + physical record sequence before delegating here.
class LogicalNodeArenaV2Reader final {
public:
    ~LogicalNodeArenaV2Reader();

    LogicalNodeArenaV2Reader(const LogicalNodeArenaV2Reader&) = delete;
    LogicalNodeArenaV2Reader& operator=(const LogicalNodeArenaV2Reader&) = delete;
    LogicalNodeArenaV2Reader(LogicalNodeArenaV2Reader&&) noexcept;
    LogicalNodeArenaV2Reader& operator=(LogicalNodeArenaV2Reader&&) noexcept;

private:
    friend class LogicalNodeArenaV2StoreBoundReader;

    explicit LogicalNodeArenaV2Reader(std::filesystem::path store_root);

    bool open(std::string* error);
    const LogicalNodeArenaV2Manifest& manifest() const noexcept;

    bool node_by_ordinal(
        std::uint64_t ordinal,
        LogicalNodeRecord* node,
        std::string* error) const;
    bool node_by_id(
        std::uint64_t logical_id,
        LogicalNodeRecord* node,
        std::string* error) const;
    bool attribute_by_ordinal(
        std::uint64_t ordinal,
        LogicalNodeAttributeRecord* attribute,
        std::string* error) const;
    bool resolve_semantic(
        LogicalSemanticKind kind,
        std::uint32_t id,
        std::string* value,
        std::string* error) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc
