#pragma once

#include "logical_node_arena_v2.hpp"
#include "logical_node_source.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace zevryon::massivedoc {

// Authoritative v2 writer. It derives the exact native-store binding itself,
// converts payload + physical record-sequence identity into a composite SHA-256
// stored by the nested arena, and publishes the binding sidecar atomically with
// the outer node-arena-v2 directory.
class LogicalNodeArenaV2StoreBoundWriter final {
public:
    LogicalNodeArenaV2StoreBoundWriter(
        std::filesystem::path store_root,
        LogicalNodeArenaBuildConfig config);
    ~LogicalNodeArenaV2StoreBoundWriter();

    LogicalNodeArenaV2StoreBoundWriter(
        const LogicalNodeArenaV2StoreBoundWriter&) = delete;
    LogicalNodeArenaV2StoreBoundWriter& operator=(
        const LogicalNodeArenaV2StoreBoundWriter&) = delete;
    LogicalNodeArenaV2StoreBoundWriter(
        LogicalNodeArenaV2StoreBoundWriter&&) noexcept;
    LogicalNodeArenaV2StoreBoundWriter& operator=(
        LogicalNodeArenaV2StoreBoundWriter&&) noexcept;

    bool begin(std::string* error);
    bool append_node(
        const LogicalNodeInput& node,
        std::span<const LogicalNodeAttributeInput> attributes,
        std::string* error);
    bool finish(std::string* error);

    std::uint64_t node_count() const noexcept;
    std::uint64_t attribute_count() const noexcept;
    std::uint64_t semantic_bucket_head_bytes() const noexcept;
    const LogicalNodeSourceStoreBinding& source_binding() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Authoritative v2 reader. open() requires the binding sidecar, compares it to
// the current native store, recomputes the composite identity, then opens the
// low-level arena and requires its frozen source hash to equal that composite.
class LogicalNodeArenaV2StoreBoundReader final {
public:
    explicit LogicalNodeArenaV2StoreBoundReader(std::filesystem::path store_root);
    ~LogicalNodeArenaV2StoreBoundReader();

    LogicalNodeArenaV2StoreBoundReader(
        const LogicalNodeArenaV2StoreBoundReader&) = delete;
    LogicalNodeArenaV2StoreBoundReader& operator=(
        const LogicalNodeArenaV2StoreBoundReader&) = delete;
    LogicalNodeArenaV2StoreBoundReader(
        LogicalNodeArenaV2StoreBoundReader&&) noexcept;
    LogicalNodeArenaV2StoreBoundReader& operator=(
        LogicalNodeArenaV2StoreBoundReader&&) noexcept;

    bool open(std::string* error);
    const LogicalNodeArenaV2Manifest& manifest() const noexcept;
    const LogicalNodeSourceStoreBinding& source_binding() const noexcept;

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
