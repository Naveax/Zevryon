#pragma once

#include "logical_node_arena.hpp"
#include "logical_node_source.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace zevryon::massivedoc {

inline constexpr std::uint32_t kLogicalNodeRecordIndexFormatVersion = 1U;
inline constexpr std::uint64_t kNoLogicalNodeRecordPosting = ~std::uint64_t{0};
inline constexpr std::size_t kMaximumLogicalNodeRecordWindowNodes = 65'536U;

struct LogicalNodeRecordIndexManifest {
    std::uint32_t format_version{0U};
    std::uint64_t source_record_count{0U};
    std::uint64_t node_count{0U};
    std::uint64_t posting_count{0U};
    std::uint64_t total_source_bytes{0U};
    LogicalNodeSourceStoreBinding source_binding{};
    std::array<std::uint8_t, 32> arena_identity_sha256{};
};

struct LogicalNodeRecordPosting {
    std::uint64_t posting_ordinal{0U};
    std::uint64_t node_ordinal{0U};
    std::uint64_t record_byte_offset{0U};
    std::uint64_t record_byte_length{0U};
};

struct LogicalNodeRecordIndexWindow {
    std::vector<LogicalNodeRecordPosting> postings;
    std::uint64_t next_posting_ordinal{kNoLogicalNodeRecordPosting};
    bool truncated{false};
};

struct LogicalNodeRecordIndexHeadSnapshot {
    std::uint64_t first_posting{kNoLogicalNodeRecordPosting};
    std::uint64_t last_posting{kNoLogicalNodeRecordPosting};
    std::uint64_t posting_count{0U};
};

// Builds a create-only, disk-backed source-record -> logical-node index beside
// the authoritative native store and store-bound node-arena-v2. Non-empty node
// source spans are indexed into every physical record they actually overlap.
// The builder keeps only bounded descriptor/node state resident; record heads
// are updated in the staging sidecar instead of retaining an O(record_count)
// RAM table. Failure never publishes the final sidecar.
bool build_logical_node_record_index(
    const std::filesystem::path& store_root,
    std::string* error);

// Low-level storage/test reader. It validates exact store/arena identity,
// posting CRCs, forward links and authoritative node/source overlap. Production
// runtime callers must use LogicalNodeRecordIndexAuthoritativeReader below so
// CRC-valid first/last/count head metadata is also enforced end-to-end.
class LogicalNodeRecordIndexReader final {
public:
    explicit LogicalNodeRecordIndexReader(std::filesystem::path store_root);
    ~LogicalNodeRecordIndexReader();

    LogicalNodeRecordIndexReader(const LogicalNodeRecordIndexReader&) = delete;
    LogicalNodeRecordIndexReader& operator=(const LogicalNodeRecordIndexReader&) = delete;
    LogicalNodeRecordIndexReader(LogicalNodeRecordIndexReader&&) noexcept;
    LogicalNodeRecordIndexReader& operator=(LogicalNodeRecordIndexReader&&) noexcept;

    bool open(std::string* error);
    const LogicalNodeRecordIndexManifest& manifest() const noexcept;
    bool read_record(
        std::uint64_t source_record_index,
        std::uint64_t continuation_posting_ordinal,
        std::size_t max_nodes,
        LogicalNodeRecordIndexWindow* result,
        std::string* error) const;

    // Reads the CRC-validated head from the exact heads.bin handle already held
    // by this reader. Production authority uses this instead of reopening the
    // path after the storage reader has established its identity.
    bool read_head_snapshot(
        std::uint64_t source_record_index,
        LogicalNodeRecordIndexHeadSnapshot* head,
        std::string* error) const;

    // Same-open-arena primitives. These use the exact store-bound arena instance
    // already held by this reader for posting overlap validation, avoiding a
    // second path reopen between identity validation and semantic materialization.
    bool node_by_ordinal(
        std::uint64_t ordinal,
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

// Production-authoritative reader. In addition to the low-level reader's exact
// store/arena/overlap checks, this layer treats every CRC-valid head field as
// authoritative: non-empty first/last/count state must be coherent,
// continuations must stay inside that record's head range and a chain that
// reaches its sentinel must terminate at the frozen last_posting ordinal.
class LogicalNodeRecordIndexAuthoritativeReader final {
public:
    explicit LogicalNodeRecordIndexAuthoritativeReader(
        std::filesystem::path store_root);
    ~LogicalNodeRecordIndexAuthoritativeReader();

    LogicalNodeRecordIndexAuthoritativeReader(
        const LogicalNodeRecordIndexAuthoritativeReader&) = delete;
    LogicalNodeRecordIndexAuthoritativeReader& operator=(
        const LogicalNodeRecordIndexAuthoritativeReader&) = delete;
    LogicalNodeRecordIndexAuthoritativeReader(
        LogicalNodeRecordIndexAuthoritativeReader&&) noexcept;
    LogicalNodeRecordIndexAuthoritativeReader& operator=(
        LogicalNodeRecordIndexAuthoritativeReader&&) noexcept;

    bool open(std::string* error);
    const LogicalNodeRecordIndexManifest& manifest() const noexcept;

    bool read_record(
        std::uint64_t source_record_index,
        std::uint64_t continuation_posting_ordinal,
        std::size_t max_nodes,
        LogicalNodeRecordIndexWindow* result,
        std::string* error) const;

    // Production semantic materialization must use these pass-throughs so the
    // exact arena instance used to validate postings also supplies node content.
    bool node_by_ordinal(
        std::uint64_t ordinal,
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
