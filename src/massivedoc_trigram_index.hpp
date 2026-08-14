#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace zevryon::massivedoc {

constexpr std::size_t kTrigramBloomBytes = 256U;
constexpr std::size_t kTrigramIntersectionStreams = 8U;

struct TrigramIndexConfig {
    std::size_t io_window_bytes{64U * 1024U};
    std::size_t sort_memory_bytes{4U * 1024U * 1024U};
    std::size_t merge_fan_in{16U};
};

struct TrigramIndexStats {
    std::uint64_t block_count{0U};
    std::uint64_t posting_pairs{0U};
    std::uint64_t distinct_trigrams{0U};
    std::uint64_t spool_bytes{0U};
    std::uint64_t index_bytes{0U};
    std::uint64_t bloom_bytes{0U};
    std::size_t peak_sort_entries{0U};
    std::uint64_t merge_passes{0U};
};

struct TrigramQueryStats {
    std::uint64_t posting_streams_opened{0U};
    std::uint64_t posting_blocks_advanced{0U};
    std::uint64_t bloom_blocks_checked{0U};
    std::uint64_t candidate_blocks_emitted{0U};
    bool cancelled{false};
};

using TrigramCancellationCheck = std::function<bool()>;
using TrigramCandidateVisitor = std::function<bool(std::uint64_t)>;

class TrigramIndexWriter {
public:
    TrigramIndexWriter(
        std::filesystem::path root,
        TrigramIndexConfig config = {});
    ~TrigramIndexWriter();

    TrigramIndexWriter(const TrigramIndexWriter&) = delete;
    TrigramIndexWriter& operator=(const TrigramIndexWriter&) = delete;

    bool begin_record(std::uint64_t block_index, std::string* error);
    bool feed(std::span<const std::byte> bytes, std::string* error);
    bool end_record(std::string* error);
    bool finish(
        std::uint64_t total_blocks,
        TrigramIndexStats* stats,
        const TrigramCancellationCheck& cancelled,
        std::string* error);

private:
    struct Impl;
    Impl* impl_{nullptr};
};

class TrigramIndexReader {
public:
    explicit TrigramIndexReader(
        std::filesystem::path root,
        TrigramIndexConfig config = {});
    ~TrigramIndexReader();

    TrigramIndexReader(const TrigramIndexReader&) = delete;
    TrigramIndexReader& operator=(const TrigramIndexReader&) = delete;

    bool open(std::string* error);
    bool visit_candidate_blocks(
        std::string_view query,
        const TrigramCandidateVisitor& visitor,
        const TrigramCancellationCheck& cancelled,
        TrigramQueryStats* stats,
        std::string* error) const;

    bool is_open() const noexcept;
    std::uint64_t block_count() const noexcept;

private:
    struct Impl;
    Impl* impl_{nullptr};
};

} // namespace zevryon::massivedoc
