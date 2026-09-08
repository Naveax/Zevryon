#pragma once

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

// Builds a create-only, disk-backed source-record -> logical-node index beside
// the authoritative native store and store-bound node-arena-v2. Non-empty node
// source spans are indexed into every physical record they actually overlap.
// The builder keeps only bounded descriptor/node state resident; record heads
// are updated in the staging sidecar instead of retaining an O(record_count)
// RAM table. Failure never publishes the final sidecar.
bool build_logical_node_record_index(
    const std::filesystem::path& store_root,
    std::string* error);

class LogicalNodeRecordIndexAuthoritativeReader;

// Low-level storage reader. It validates store/arena identity, posting CRCs,
// forward links and exact node/source overlap. Construction and queries are
// intentionally private so production callers cannot bypass the additional
// head-chain authority enforced by LogicalNodeRecordIndexAuthoritativeReader.
class LogicalNodeRecordIndexReader final {
private:
    friend class LogicalNodeRecordIndexAuthoritativeReader;

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

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Public production reader. In addition to the low-level reader's exact
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc
