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

// V2 is published under <store_root>/node-arena-v2/ and intentionally keeps
// the admitted v1 arena storage inside node-arena-v2/node-arena/. A wrapper
// manifest binds that storage to the v2 cross-record source-span contract.
// This layout prevents an old LogicalNodeArenaReader(store_root) from silently
// opening v2 data with v1 source-range semantics.
class LogicalNodeArenaV2Writer {
public:
    LogicalNodeArenaV2Writer(
        std::filesystem::path store_root,
        LogicalNodeArenaBuildConfig config);
    ~LogicalNodeArenaV2Writer();

    LogicalNodeArenaV2Writer(const LogicalNodeArenaV2Writer&) = delete;
    LogicalNodeArenaV2Writer& operator=(const LogicalNodeArenaV2Writer&) = delete;
    LogicalNodeArenaV2Writer(LogicalNodeArenaV2Writer&&) noexcept;
    LogicalNodeArenaV2Writer& operator=(LogicalNodeArenaV2Writer&&) noexcept;

    bool begin(std::string* error);
    bool append_node(
        const LogicalNodeInput& node,
        std::span<const LogicalNodeAttributeInput> attributes,
        std::string* error);
    bool finish(std::string* error);

    std::uint64_t node_count() const noexcept;
    std::uint64_t attribute_count() const noexcept;
    std::uint64_t semantic_bucket_head_bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class LogicalNodeArenaV2Reader {
public:
    explicit LogicalNodeArenaV2Reader(std::filesystem::path store_root);
    ~LogicalNodeArenaV2Reader();

    LogicalNodeArenaV2Reader(const LogicalNodeArenaV2Reader&) = delete;
    LogicalNodeArenaV2Reader& operator=(const LogicalNodeArenaV2Reader&) = delete;
    LogicalNodeArenaV2Reader(LogicalNodeArenaV2Reader&&) noexcept;
    LogicalNodeArenaV2Reader& operator=(LogicalNodeArenaV2Reader&&) noexcept;

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc
