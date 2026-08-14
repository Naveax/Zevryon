#include "massivedoc_block_cache.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
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

std::vector<std::byte> source_bytes(std::size_t count) {
    std::vector<std::byte> bytes(count);
    for (std::size_t index = 0U; index < count; ++index) {
        bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(index & 0xffU));
    }
    return bytes;
}

zevryon::massivedoc::ImmutableBlockLoader loader_for(
    const std::vector<std::byte>* source) {
    return [source](
               std::uint32_t source_id,
               std::uint64_t block_offset,
               std::size_t maximum_bytes,
               std::vector<std::byte>* output,
               std::string* error) {
        if (source_id != 7U || output == nullptr || error == nullptr) {
            if (error != nullptr) {
                *error = "invalid synthetic block loader request";
            }
            return false;
        }
        if (block_offset >= static_cast<std::uint64_t>(source->size())) {
            *error = "synthetic block loader offset out of range";
            return false;
        }
        const std::size_t offset = static_cast<std::size_t>(block_offset);
        const std::size_t amount = std::min(maximum_bytes, source->size() - offset);
        output->assign(
            source->begin() + static_cast<std::ptrdiff_t>(offset),
            source->begin() + static_cast<std::ptrdiff_t>(offset + amount));
        error->clear();
        return true;
    };
}

void expect_read(
    zevryon::massivedoc::ImmutableBlockCache* cache,
    const zevryon::massivedoc::ImmutableBlockLoader& loader,
    std::uint64_t offset,
    std::size_t amount) {
    std::vector<std::byte> output(amount);
    std::string error;
    require(
        cache->read_exact(7U, offset, output, loader, &error),
        error);
    for (std::size_t index = 0U; index < amount; ++index) {
        const auto expected = static_cast<std::byte>(
            static_cast<unsigned char>((offset + index) & 0xffU));
        require(output[index] == expected, "immutable block cache returned wrong byte");
    }
}

void test_hot_warm_cold_admission_and_eviction() {
    const auto source = source_bytes(32U);
    const auto loader = loader_for(&source);
    zevryon::massivedoc::ImmutableBlockCache cache({4U, 4U, 8U});

    expect_read(&cache, loader, 0U, 4U);
    auto stats = cache.stats();
    require(stats.cold_misses == 1U, "first block was not a cold miss");
    require(stats.warm_blocks == 1U && stats.hot_blocks == 0U, "cold block was not admitted warm");
    require(stats.resident_bytes == 4U, "warm admission resident bytes mismatch");

    expect_read(&cache, loader, 0U, 4U);
    stats = cache.stats();
    require(stats.warm_hits == 1U, "second access was not a warm hit");
    require(stats.promotions == 1U, "warm block was not promoted hot");
    require(stats.hot_blocks == 1U && stats.warm_blocks == 0U, "hot promotion tier mismatch");

    expect_read(&cache, loader, 4U, 4U);
    expect_read(&cache, loader, 4U, 4U);
    stats = cache.stats();
    require(stats.promotions == 2U, "second warm block was not promoted hot");
    require(stats.demotions == 1U, "hot budget did not demote the older hot block");
    require(stats.hot_blocks == 1U && stats.warm_blocks == 1U, "hot/warm demotion shape mismatch");

    expect_read(&cache, loader, 8U, 4U);
    expect_read(&cache, loader, 12U, 4U);
    stats = cache.stats();
    require(stats.evictions == 1U, "warm budget did not evict to cold");
    require(stats.resident_bytes <= 12U, "resident cache exceeded configured byte budget");
    require(stats.peak_resident_bytes <= 12U, "peak resident cache exceeded configured byte budget");

    expect_read(&cache, loader, 4U, 4U);
    stats = cache.stats();
    require(stats.hot_hits == 1U, "hot block did not hit without physical read");
    require(stats.physical_read_bytes == 16U, "unexpected physical reads before cold re-entry");

    expect_read(&cache, loader, 0U, 4U);
    stats = cache.stats();
    require(stats.cold_misses == 5U, "evicted block did not re-enter from cold");
    require(stats.evictions == 2U, "cold re-entry did not preserve warm byte bound");
    require(stats.physical_read_bytes == 20U, "cold re-entry physical read accounting mismatch");
    require(stats.ledger_within_hard_limits, "SourceWindow ledger exceeded hard limit");
    require(stats.ledger_accounting_clean, "SourceWindow ledger accounting is dirty");

    cache.evict_all_to_cold();
    stats = cache.stats();
    require(stats.resident_bytes == 0U, "evict-all did not return all resident blocks to cold");
    require(stats.ledger_within_hard_limits, "ledger invalid after evict-all");
    require(stats.ledger_accounting_clean, "ledger accounting invalid after evict-all");
}

void test_cross_block_reads_preserve_source_order() {
    const auto source = source_bytes(32U);
    const auto loader = loader_for(&source);
    zevryon::massivedoc::ImmutableBlockCache cache({4U, 8U, 8U});
    expect_read(&cache, loader, 3U, 10U);
    const auto stats = cache.stats();
    require(stats.cold_misses == 4U, "cross-block read did not traverse expected immutable blocks");
    require(stats.physical_read_bytes == 16U, "cross-block physical read accounting mismatch");
    require(stats.resident_bytes <= 16U, "cross-block read exceeded resident budget");
}

void test_disabled_cache_remains_cold_and_nonresident() {
    const auto source = source_bytes(16U);
    const auto loader = loader_for(&source);
    zevryon::massivedoc::ImmutableBlockCache cache({4U, 0U, 0U});
    expect_read(&cache, loader, 0U, 4U);
    expect_read(&cache, loader, 0U, 4U);
    const auto stats = cache.stats();
    require(stats.cold_misses == 2U, "disabled cache unexpectedly retained a block");
    require(stats.hot_hits == 0U && stats.warm_hits == 0U, "disabled cache reported a resident hit");
    require(stats.resident_bytes == 0U, "disabled cache retained resident bytes");
    require(stats.physical_read_bytes == 8U, "disabled cache physical read accounting mismatch");
}

void test_concurrent_hits_share_one_thread_safe_ledger() {
    const auto source = source_bytes(16U);
    const auto loader = loader_for(&source);
    zevryon::massivedoc::ImmutableBlockCache cache({8U, 8U, 8U});
    constexpr std::size_t kThreads = 16U;
    std::array<bool, kThreads> succeeded{};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t index = 0U; index < kThreads; ++index) {
        threads.emplace_back([&cache, &loader, &succeeded, index] {
            std::array<std::byte, 8> output{};
            std::string error;
            succeeded[index] = cache.read_exact(7U, 0U, output, loader, &error);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const bool success : succeeded) {
        require(success, "concurrent immutable block cache read failed");
    }
    const auto stats = cache.stats();
    require(stats.cold_misses == 1U, "concurrent reads issued duplicate cold loads");
    require(stats.warm_hits == 1U, "concurrent reads did not perform one warm promotion");
    require(stats.hot_hits == 14U, "concurrent reads did not reuse the promoted hot block");
    require(stats.physical_read_bytes == 8U, "concurrent reads duplicated physical I/O");
    require(stats.ledger_within_hard_limits, "concurrent ledger exceeded hard limit");
    require(stats.ledger_accounting_clean, "concurrent ledger accounting is dirty");
}

void test_config_validation() {
    std::string error;
    require(
        !zevryon::massivedoc::validate_immutable_block_cache_config({0U, 0U, 0U}, &error),
        "zero immutable block size was accepted");
    require(
        !zevryon::massivedoc::validate_immutable_block_cache_config({64U, 32U, 64U}, &error),
        "undersized hot budget was accepted");
    require(
        !zevryon::massivedoc::validate_immutable_block_cache_config({64U, 64U, 0U}, &error),
        "hot cache without warm admission tier was accepted");
    require(
        !zevryon::massivedoc::validate_immutable_block_cache_config(
            {64U, zevryon::massivedoc::kMaximumResidentBlockCacheBytes, 64U},
            &error),
        "resident cache hard-limit overflow was accepted");
    require(
        zevryon::massivedoc::validate_immutable_block_cache_config({64U, 64U, 128U}, &error),
        error);
}

} // namespace

int main() {
    test_hot_warm_cold_admission_and_eviction();
    test_cross_block_reads_preserve_source_order();
    test_disabled_cache_remains_cold_and_nonresident();
    test_concurrent_hits_share_one_thread_safe_ledger();
    test_config_validation();
    std::cout << "Zevryon MassiveDoc immutable block cache tests passed\n";
    return 0;
}
