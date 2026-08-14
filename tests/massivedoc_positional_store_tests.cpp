#include "massivedoc_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

std::filesystem::path temp_root(std::string_view name) {
    std::mt19937_64 random(0x4d3353544f524549ULL);
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("zevryon-") + std::string(name) + "-" +
                       std::to_string(random()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "cannot clear positional store test root");
    return root;
}

void cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "positional store test cleanup failed");
}

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(
        begin,
        begin + static_cast<std::ptrdiff_t>(text.size()));
}

void create_store(const std::filesystem::path& root) {
    zevryon::massivedoc::StoreConfig config;
    config.segment_bytes = 11U;
    config.records_per_search_block = 2U;
    zevryon::massivedoc::StoreWriter writer(root, config);
    std::string error;
    const auto first = bytes_of("alpha-record-crosses-segments");
    const auto second = bytes_of("beta-record-also-crosses-segments");
    require(writer.append(1001U, first, &error), error);
    require(writer.append(1002U, second, &error), error);
    zevryon::massivedoc::StoreStats stats;
    require(writer.finalize({}, &stats, &error), error);
    require(stats.corpus.logical_records == 2U, "fixture record count mismatch");
}

zevryon::massivedoc::StoreReadConfig tiny_cached_config() {
    zevryon::massivedoc::StoreReadConfig config;
    config.io_window_bytes = 3U;
    config.block_cache = zevryon::massivedoc::ImmutableBlockCacheConfig{4U, 4U, 8U};
    return config;
}

void test_store_reader_uses_tiny_bounded_positional_window() {
    const auto root = temp_root("positional-store-window");
    create_store(root);

    zevryon::massivedoc::StoreReader reader(root, tiny_cached_config());
    std::string error;
    require(reader.open(&error), error);
    require(reader.verify(&error), error);

    std::vector<std::byte> slice;
    require(reader.read_record_slice(0U, 6U, 12U, &slice, &error), error);
    const auto* slice_data = reinterpret_cast<const char*>(slice.data());
    require(
        std::string_view(slice_data, slice.size()) == "record-cross",
        "tiny-window record slice mismatch");

    std::string collected;
    require(
        reader.read_record(
            1U,
            [&collected](std::span<const std::byte> bytes) {
                collected.append(
                    reinterpret_cast<const char*>(bytes.data()),
                    bytes.size());
                return true;
            },
            &error),
        error);
    require(
        collected == "beta-record-also-crosses-segments",
        "tiny-window full record mismatch");
    const auto cache = reader.block_cache_stats();
    require(cache.resident_bytes <= 12U, "tiny-window store cache exceeded resident budget");
    require(cache.ledger_within_hard_limits, "tiny-window store cache exceeded ledger hard limit");
    require(cache.ledger_accounting_clean, "tiny-window store cache ledger accounting is dirty");
    cleanup(root);
}

void test_store_reader_shares_segment_cache_across_slice_and_search() {
    const auto root = temp_root("positional-store-shared-cache");
    create_store(root);

    zevryon::massivedoc::StoreReader reader(root, tiny_cached_config());
    std::string error;
    require(reader.open(&error), error);

    std::vector<std::byte> slice;
    require(reader.read_record_slice(0U, 0U, 4U, &slice, &error), error);
    require(
        std::string_view(reinterpret_cast<const char*>(slice.data()), slice.size()) == "alph",
        "cached slice content mismatch");
    auto stats = reader.block_cache_stats();
    require(stats.cold_misses == 1U, "first store payload block was not a cold miss");
    require(stats.physical_read_bytes == 4U, "first store payload block physical read mismatch");
    require(stats.warm_blocks == 1U, "first store payload block was not admitted warm");

    slice.clear();
    require(reader.read_record_slice(0U, 0U, 4U, &slice, &error), error);
    stats = reader.block_cache_stats();
    require(stats.warm_hits == 1U, "second store payload block access was not warm");
    require(stats.promotions == 1U && stats.hot_blocks == 1U, "store payload block was not promoted hot");
    require(stats.physical_read_bytes == 4U, "resident store payload block was physically reread");

    const auto hits = reader.find("alpha", 1U, &error);
    require(error.empty(), error);
    require(hits.size() == 1U && hits[0].record_index == 0U && hits[0].byte_offset == 0U,
            "cached search result mismatch");
    stats = reader.block_cache_stats();
    require(stats.hot_hits >= 1U, "search did not reuse the hot payload block from slice read");
    require(stats.physical_read_bytes == 8U,
            "slice-to-search cache sharing did not avoid duplicate physical block read");
    require(stats.resident_bytes <= 12U, "shared store cache exceeded configured resident budget");

    std::string collected;
    require(
        reader.read_record(
            0U,
            [&collected](std::span<const std::byte> bytes) {
                collected.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                return true;
            },
            &error),
        error);
    require(collected == "alpha-record-crosses-segments", "cached full-record content mismatch");
    stats = reader.block_cache_stats();
    require(stats.demotions > 0U, "store payload hot budget never demoted a block");
    require(stats.evictions > 0U, "store payload warm budget never evicted a block to cold");
    require(stats.peak_resident_bytes <= 12U, "store payload cache peak exceeded byte budget");
    require(stats.ledger_within_hard_limits, "shared store cache exceeded ledger hard limit");
    require(stats.ledger_accounting_clean, "shared store cache ledger accounting is dirty");

    reader.evict_block_cache_to_cold();
    stats = reader.block_cache_stats();
    require(stats.resident_bytes == 0U, "store cache did not evict all resident payload to cold");
    require(stats.ledger_accounting_clean, "store cache ledger dirty after evict-all");
    cleanup(root);
}

void test_store_reader_rejects_unbounded_window_configs() {
    const auto root = temp_root("positional-store-bounds");
    create_store(root);

    std::string error;
    zevryon::massivedoc::StoreReadConfig zero_config;
    zero_config.io_window_bytes = 0U;
    zevryon::massivedoc::StoreReader zero_reader(root, zero_config);
    require(!zero_reader.open(&error), "zero store read window was accepted");
    require(
        error == "store reader I/O window is outside the supported bounded range",
        "zero store read window diagnostic mismatch");

    zevryon::massivedoc::StoreReadConfig oversized_config;
    oversized_config.io_window_bytes = zevryon::massivedoc::kMaximumIoWindowBytes + 1U;
    zevryon::massivedoc::StoreReader oversized_reader(root, oversized_config);
    require(!oversized_reader.open(&error), "oversized store read window was accepted");
    require(
        error == "store reader I/O window is outside the supported bounded range",
        "oversized store read window diagnostic mismatch");

    zevryon::massivedoc::StoreReadConfig invalid_cache_config;
    invalid_cache_config.block_cache = zevryon::massivedoc::ImmutableBlockCacheConfig{4U, 4U, 0U};
    zevryon::massivedoc::StoreReader invalid_cache_reader(root, invalid_cache_config);
    require(!invalid_cache_reader.open(&error), "invalid store block cache config was accepted");
    require(
        error.rfind("store reader immutable block cache config is invalid:", 0U) == 0U,
        "invalid store block cache diagnostic mismatch");
    cleanup(root);
}

} // namespace

int main() {
    test_store_reader_uses_tiny_bounded_positional_window();
    test_store_reader_shares_segment_cache_across_slice_and_search();
    test_store_reader_rejects_unbounded_window_configs();
    std::cout << "Zevryon MassiveDoc positional store tests passed\n";
    return 0;
}
