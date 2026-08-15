#pragma once

#include "massivedoc_block_cache.hpp"
#include "massivedoc_cold_window.hpp"
#include "massivedoc_generation.hpp"
#include "massivedoc_positional_io.hpp"
#include "massivedoc_trigram_index.hpp"
#include "massivedoc_unicode_search_runtime.hpp"

#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
#include "massivedoc_descriptor_shadow.hpp"
#endif

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zevryon::massivedoc {

struct ResourceContract {
    std::size_t ingest_buffer_bytes{256U * 1024U};
    std::size_t hot_cache_bytes{16U * 1024U * 1024U};
    std::size_t cold_cache_bytes{16U * 1024U * 1024U};
    std::uint64_t logical_address_limit{1ULL << 40U};
};

struct StoreConfig {
    std::size_t chunk_bytes{4U * 1024U * 1024U};
    std::size_t segment_bytes{64U * 1024U * 1024U};
    std::size_t records_per_search_block{256U};
    ResourceContract contract{};
};

struct StoreReadConfig {
    std::size_t io_window_bytes{64U * 1024U};
    std::size_t hot_cache_bytes{0U};
    std::size_t cold_cache_bytes{0U};
};

struct CorpusStats {
    std::uint64_t logical_records{0};
    std::uint64_t logical_bytes{0};
};

struct StoreStats {
    CorpusStats corpus{};
    std::uint64_t chunk_count{0};
    std::uint64_t segment_count{0};
    std::uint64_t search_block_count{0};
    std::uint64_t physical_store_bytes{0};
};

struct SearchHit {
    std::uint64_t record_index{0};
    std::uint64_t logical_id{0};
    std::uint64_t byte_offset{0};
};

using RecordConsumer = std::function<bool(std::span<const std::byte>)>;
using SearchCancel = std::function<bool()>;

struct SearchExecutionStats {
    bool used_trigram{false};
    bool fell_back_from_trigram{false};
    bool cancelled{false};
    std::uint64_t search_blocks_visited{0};
    std::uint64_t record_candidates_visited{0};
    std::uint64_t exact_records_scanned{0};
};

struct UnicodeSearchOptions {
    std::size_t max_query_bytes{64U * 1024U};
    std::size_t max_query_codepoints{4096U};
    std::size_t max_pending_codepoints{256U};
};

struct UnicodeSearchExecutionStats {
    bool cancelled{false};
    std::uint64_t records_scanned{0U};
    std::uint64_t source_bytes_decoded{0U};
    std::uint64_t normalized_codepoints{0U};
    std::uint64_t query_normalized_codepoints{0U};
};

bool validate_config(const StoreConfig& config, std::string* error);
bool validate_read_config(const StoreReadConfig& config, std::string* error);
StoreReadConfig default_store_read_config() noexcept;
std::uint64_t process_resident_bytes();

class StoreWriter {
public:
    StoreWriter(std::filesystem::path root, StoreConfig config);
    ~StoreWriter();

    StoreWriter(const StoreWriter&) = delete;
    StoreWriter& operator=(const StoreWriter&) = delete;

    bool append(
        std::uint64_t logical_id,
        std::span<const std::byte> payload,
        std::string* error);
    bool finalize(
        const CorpusStats& expected,
        StoreStats* stats,
        std::string* error);

private:
    struct Impl;
    Impl* impl_;
};

class StoreReader {
public:
    explicit StoreReader(
        std::filesystem::path root,
        StoreReadConfig read_config = default_store_read_config());
    ~StoreReader();

    StoreReader(const StoreReader&) = delete;
    StoreReader& operator=(const StoreReader&) = delete;

    bool open(std::string* error);
    bool read_record(std::uint64_t record_index, const RecordConsumer& consumer, std::string* error) const;
    bool read_record_slice(
        std::uint64_t record_index,
        std::uint64_t byte_offset,
        std::size_t max_bytes,
        std::vector<std::byte>* output,
        std::string* error) const;
    bool verify(std::string* error) const;
    bool export_payload(const std::filesystem::path& output, std::string* error) const;
    std::vector<SearchHit> find(
        std::string_view query,
        std::size_t max_hits,
        std::string* error) const;
    std::vector<SearchHit> find_bounded(
        std::string_view query,
        std::size_t max_hits,
        const SearchCancel& cancel,
        SearchExecutionStats* execution,
        std::string* error) const;
    std::vector<SearchHit> find_unicode_bounded(
        std::string_view query_utf8,
        std::size_t max_hits,
        const SearchCancel& cancel,
        const UnicodeSearchOptions& options,
        UnicodeSearchExecutionStats* execution,
        std::string* error) const;
    const StoreStats& stats() const;
    CacheStats cache_stats() const noexcept;
    ColdWindowStats cold_window_stats() const noexcept;
    std::uint64_t cache_resident_bytes() const noexcept;
    std::uint64_t cache_peak_resident_bytes() const noexcept;
    std::uint64_t cache_physical_read_bytes() const noexcept;
    std::uint64_t cold_window_resident_bytes() const noexcept;
    std::uint64_t cold_window_peak_resident_bytes() const noexcept;
    std::uint64_t cold_window_touched_bytes() const noexcept;
    bool cache_ledger_within_hard_limits() const noexcept;
    bool cache_ledger_accounting_clean() const noexcept;
    bool cold_window_ledger_within_hard_limits() const noexcept;
    bool cold_window_ledger_accounting_clean() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

std::string stats_json(const StoreStats& stats, std::uint64_t resident_bytes);

} // namespace zevryon::massivedoc
