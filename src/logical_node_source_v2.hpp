#pragma once

#include "logical_node_source.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace zevryon::massivedoc {

inline constexpr std::uint32_t kLogicalNodeSourceV2FormatVersion = 2U;

// ZVNSRC01 v2 preserves the v1 binary header/frame sizes but changes source
// range semantics. A node source span is identified by
// (source_record_index, source_byte_offset, source_byte_length), where length is
// the total contiguous logical byte count and may continue through subsequent
// physical records. The physical record sequence remains cryptographically
// bound by LogicalNodeSourceStoreBinding::record_sequence_sha256.
class LogicalNodeSourceV2Writer {
public:
    explicit LogicalNodeSourceV2Writer(std::filesystem::path output_path);
    ~LogicalNodeSourceV2Writer();

    LogicalNodeSourceV2Writer(const LogicalNodeSourceV2Writer&) = delete;
    LogicalNodeSourceV2Writer& operator=(const LogicalNodeSourceV2Writer&) = delete;
    LogicalNodeSourceV2Writer(LogicalNodeSourceV2Writer&&) noexcept;
    LogicalNodeSourceV2Writer& operator=(LogicalNodeSourceV2Writer&&) noexcept;

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

class LogicalNodeSourceV2Reader {
public:
    explicit LogicalNodeSourceV2Reader(std::filesystem::path source_path);
    ~LogicalNodeSourceV2Reader();

    LogicalNodeSourceV2Reader(const LogicalNodeSourceV2Reader&) = delete;
    LogicalNodeSourceV2Reader& operator=(const LogicalNodeSourceV2Reader&) = delete;
    LogicalNodeSourceV2Reader(LogicalNodeSourceV2Reader&&) noexcept;
    LogicalNodeSourceV2Reader& operator=(LogicalNodeSourceV2Reader&&) noexcept;

    bool open(std::string* error);
    const LogicalNodeSourceManifest& manifest() const noexcept;
    bool next(LogicalNodeSourceNode* node, bool* has_node, std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct LogicalNodeSourceV2ValidationStats {
    std::uint64_t nodes_validated{0U};
    std::uint64_t attributes_validated{0U};
    std::uint64_t source_span_bytes_streamed{0U};
    std::uint64_t zero_length_spans{0U};
};

// Validates an untrusted v2 source against the authoritative native store.
// Cross-record ranges are checked with StoreReader::read_record_span(), so no
// complete source span is materialized in memory. This intentionally performs
// a bounded second read pass over non-empty spans until a descriptor-only span
// validator is admitted separately.
bool validate_logical_node_source_v2_against_store(
    const std::filesystem::path& source_path,
    const std::filesystem::path& store_root,
    LogicalNodeSourceV2ValidationStats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
