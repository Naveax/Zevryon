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

inline constexpr std::uint32_t kLogicalNodeSourceRecordIndexFormatVersion = 1U;
inline constexpr std::uint64_t kNoLogicalNodeSourceRecordPosting = ~std::uint64_t{0};
inline constexpr std::size_t kMaximumLogicalNodeSourceRecordPagePostings = 65'536U;

struct LogicalNodeSourceRecordIndexManifest {
    std::uint32_t format_version{0U};
    std::uint64_t source_record_count{0U};
    std::uint64_t node_count{0U};
    std::uint64_t posting_count{0U};
    LogicalNodeSourceStoreBinding source_binding{};
    std::array<std::uint8_t, 32> arena_identity_sha256{};
};

struct LogicalNodeSourceRecordIndexBuildStats {
    std::uint64_t nodes_scanned{0U};
    std::uint64_t source_nodes_indexed{0U};
    std::uint64_t postings_written{0U};
    std::uint64_t maximum_records_per_node{0U};
};

struct LogicalNodeSourceRecordPosting {
    std::uint64_t node_ordinal{0U};
    std::uint64_t source_record_index{0U};
    std::uint64_t source_byte_offset{0U};
    std::uint64_t source_byte_length{0U};
    std::uint64_t node_span_offset{0U};
};

struct LogicalNodeSourceRecordPage {
    std::uint64_t source_record_index{0U};
    std::uint64_t total_postings_for_record{0U};
    std::uint64_t next_posting_cursor{kNoLogicalNodeSourceRecordPosting};
    bool truncated{false};
    std::vector<LogicalNodeSourceRecordPosting> postings;
};

// Builds a create-only disk-backed relationship from physical source records to
// every logical-node source span that overlaps those records. The authoritative
// store-bound arena v2 remains the node authority; this index never equates a
// record logical_id with a browser-node logical_id/ordinal.
bool build_logical_node_source_record_index_v1(
    const std::filesystem::path& store_root,
    LogicalNodeSourceRecordIndexBuildStats* stats,
    std::string* error);

class LogicalNodeSourceRecordIndexV1Reader final {
public:
    explicit LogicalNodeSourceRecordIndexV1Reader(std::filesystem::path store_root);
    ~LogicalNodeSourceRecordIndexV1Reader();

    LogicalNodeSourceRecordIndexV1Reader(
        const LogicalNodeSourceRecordIndexV1Reader&) = delete;
    LogicalNodeSourceRecordIndexV1Reader& operator=(
        const LogicalNodeSourceRecordIndexV1Reader&) = delete;
    LogicalNodeSourceRecordIndexV1Reader(
        LogicalNodeSourceRecordIndexV1Reader&&) noexcept;
    LogicalNodeSourceRecordIndexV1Reader& operator=(
        LogicalNodeSourceRecordIndexV1Reader&&) noexcept;

    bool open(std::string* error);
    const LogicalNodeSourceRecordIndexManifest& manifest() const noexcept;

    // kNoLogicalNodeSourceRecordPosting starts at the record head. A returned
    // next_posting_cursor can be passed back to continue a bounded page without
    // materializing the complete posting list. max_postings has a hard ceiling.
    bool read_record_page(
        std::uint64_t source_record_index,
        std::uint64_t start_posting_cursor,
        std::size_t max_postings,
        LogicalNodeSourceRecordPage* page,
        std::string* error) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc
