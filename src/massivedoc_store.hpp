#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::massivedoc {

constexpr std::uint64_t kDefaultSegmentBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kDefaultRecordsPerSearchBlock = 8192U;
constexpr std::size_t kBigramSignatureBytes = 8192U;
constexpr std::size_t kIoWindowBytes = 1024U * 1024U;

struct CorpusMetadata {
    std::uint64_t logical_utf8_bytes{0};
    std::uint64_t logical_records{0};
    std::uint64_t logical_nodes{0};
    std::uint64_t style_runs{0};
    std::uint64_t resource_references{0};
    std::uint64_t largest_record_bytes{0};
};

struct StoreConfig {
    std::uint64_t segment_bytes{kDefaultSegmentBytes};
    std::uint32_t records_per_search_block{kDefaultRecordsPerSearchBlock};
};

struct StoreStats {
    CorpusMetadata corpus;
    std::uint64_t segment_count{0};
    std::uint64_t chunk_count{0};
    std::uint64_t search_block_count{0};
    std::uint64_t physical_bytes{0};
    std::string payload_sha256;
};

struct SearchHit {
    std::uint64_t record_index{0};
    std::uint64_t logical_id{0};
    std::uint64_t byte_offset{0};
};

class StoreWriter {
public:
    StoreWriter(const std::filesystem::path& root, StoreConfig config = {});
    ~StoreWriter();

    StoreWriter(const StoreWriter&) = delete;
    StoreWriter& operator=(const StoreWriter&) = delete;

    bool append(std::uint64_t logical_id, std::span<const std::byte> payload, std::string* error);
    bool append_stream(
        std::uint64_t logical_id,
        std::uint64_t length,
        const std::function<std::size_t(std::span<std::byte>)>& reader,
        std::string* error);
    bool finalize(CorpusMetadata metadata, StoreStats* stats, std::string* error);

private:
    struct Impl;
    Impl* impl_{nullptr};
};

class StoreReader {
public:
    explicit StoreReader(const std::filesystem::path& root);
    ~StoreReader();

    StoreReader(const StoreReader&) = delete;
    StoreReader& operator=(const StoreReader&) = delete;

    bool open(std::string* error);
    const StoreStats& stats() const noexcept;
    bool verify(std::string* error) const;
    bool export_payload(const std::filesystem::path& output, std::string* error) const;
    std::vector<SearchHit> find(std::string_view query, std::size_t max_hits, std::string* error) const;
    bool read_record(
        std::uint64_t record_index,
        const std::function<bool(std::span<const std::byte>)>& consumer,
        std::string* error) const;
    bool read_record_slice(
        std::uint64_t record_index,
        std::uint64_t byte_offset,
        std::size_t max_bytes,
        std::vector<std::byte>* output,
        std::string* error) const;

private:
    struct Impl;
    Impl* impl_{nullptr};
};

bool import_zmdoc_corpus(
    const std::filesystem::path& corpus_path,
    const std::filesystem::path& store_root,
    StoreConfig config,
    StoreStats* stats,
    std::string* error);

std::string stats_json(const StoreStats& stats);

} // namespace zevryon::massivedoc
