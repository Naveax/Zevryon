#include "verified_font_resource_cache.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace zevryon::text;
using Bytes = std::vector<std::byte>;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

void put_u16(Bytes* bytes, std::size_t offset, std::uint16_t value) {
    (*bytes)[offset] = std::byte{static_cast<unsigned char>(value >> 8U)};
    (*bytes)[offset + 1U] = std::byte{static_cast<unsigned char>(value)};
}

void put_u32(Bytes* bytes, std::size_t offset, std::uint32_t value) {
    (*bytes)[offset] = std::byte{static_cast<unsigned char>(value >> 24U)};
    (*bytes)[offset + 1U] =
        std::byte{static_cast<unsigned char>(value >> 16U)};
    (*bytes)[offset + 2U] =
        std::byte{static_cast<unsigned char>(value >> 8U)};
    (*bytes)[offset + 3U] = std::byte{static_cast<unsigned char>(value)};
}

void put_record(
    Bytes* bytes,
    std::size_t offset,
    std::uint32_t tag,
    std::uint32_t checksum_value,
    std::uint32_t table_offset,
    std::uint32_t length) {
    put_u32(bytes, offset, tag);
    put_u32(bytes, offset + 4U, checksum_value);
    put_u32(bytes, offset + 8U, table_offset);
    put_u32(bytes, offset + 12U, length);
}

std::uint32_t checksum(
    std::span<const std::byte> bytes,
    std::size_t zero_offset = 0U,
    std::size_t zero_length = 0U) {
    std::uint32_t sum = 0U;
    for (std::size_t offset = 0U; offset < bytes.size(); offset += 4U) {
        std::uint32_t word = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            const std::size_t current = offset + index;
            unsigned char value = 0U;
            if (current < bytes.size()) {
                const bool zeroed = zero_length != 0U &&
                    current >= zero_offset &&
                    current - zero_offset < zero_length;
                if (!zeroed) {
                    value = std::to_integer<unsigned char>(bytes[current]);
                }
            }
            word |= static_cast<std::uint32_t>(value)
                    << static_cast<unsigned int>((3U - index) * 8U);
        }
        sum += word;
    }
    return sum;
}

Bytes make_font() {
    constexpr std::size_t kCmapOffset = 64U;
    constexpr std::size_t kHeadOffset = 72U;
    constexpr std::size_t kMaxpOffset = 84U;
    Bytes bytes(92U, std::byte{0});
    put_u32(&bytes, 0U, 0x00010000U);
    put_u16(&bytes, 4U, 3U);
    put_u16(&bytes, 6U, 32U);
    put_u16(&bytes, 8U, 1U);
    put_u16(&bytes, 10U, 16U);
    for (std::size_t index = 0U; index < 5U; ++index) {
        bytes[kCmapOffset + index] =
            std::byte{static_cast<unsigned char>(0x10U + index)};
    }
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[kHeadOffset + index] =
            std::byte{static_cast<unsigned char>(0x20U + index)};
    }
    for (std::size_t index = 0U; index < 6U; ++index) {
        bytes[kMaxpOffset + index] =
            std::byte{static_cast<unsigned char>(0x30U + index)};
    }
    const auto all = std::span<const std::byte>(bytes);
    put_record(
        &bytes,
        12U,
        sfnt_tag('c', 'm', 'a', 'p'),
        checksum(all.subspan(kCmapOffset, 5U)),
        64U,
        5U);
    put_record(
        &bytes,
        28U,
        sfnt_tag('h', 'e', 'a', 'd'),
        checksum(all.subspan(kHeadOffset, 12U), 8U, 4U),
        72U,
        12U);
    put_record(
        &bytes,
        44U,
        sfnt_tag('m', 'a', 'x', 'p'),
        checksum(all.subspan(kMaxpOffset, 6U)),
        84U,
        6U);
    put_u32(&bytes, kHeadOffset + 8U, kSfntWholeFontChecksum - checksum(bytes));
    return bytes;
}

VerifiedFontResourceCacheKey key(
    std::uint64_t high,
    std::uint64_t low) noexcept {
    return VerifiedFontResourceCacheKey{high, low, 0U};
}

bool hit_miss_and_collision() {
    const Bytes font = make_font();
    VerifiedFontResourceCache cache(font.size() * 4U, 64U * 1024U, 4U);
    std::shared_ptr<const VerifiedFontResource> first;
    VerifiedFontResourceCacheStats stats;
    VerifiedFontResourceCacheError error;

    bool ok = expect(
        !cache.lookup(key(1U, 1U), &first, &stats, &error),
        "empty cache lookup must miss");
    ok &= expect(!first &&
                     error.kind == VerifiedFontResourceCacheErrorKind::CacheMiss &&
                     stats.misses == 1U,
                 "lookup miss must be exact and atomic");

    ok &= expect(
        cache.get_or_build(key(1U, 1U), font, &first, &stats, &error),
        "valid source must populate the cache");
    if (!first) {
        return false;
    }
    const std::uint64_t first_id = first->resource_id();
    ok &= expect(stats.build_attempts == 1U &&
                     stats.builds_published == 1U &&
                     stats.entry_count == 1U &&
                     stats.retention.current_bytes == font.size(),
                 "first population must publish one exactly accounted entry");

    std::shared_ptr<const VerifiedFontResource> hit;
    ok &= expect(
        cache.lookup(key(1U, 1U), &hit, &stats, &error),
        "resident lookup must hit");
    ok &= expect(hit == first && hit->resource_id() == first_id &&
                     stats.hits == 1U,
                 "lookup must return the identical immutable handle");

    Bytes different = font;
    different[64U] ^= std::byte{1U};
    std::shared_ptr<const VerifiedFontResource> collision;
    ok &= expect(
        !cache.get_or_build(
            key(1U, 1U), different, &collision, &stats, &error),
        "same key with different bytes must fail closed");
    ok &= expect(!collision &&
                     error.kind == VerifiedFontResourceCacheErrorKind::KeyCollision &&
                     stats.key_collisions == 1U &&
                     stats.entry_count == 1U,
                 "key collision must not replace the resident resource");
    ok &= expect(stats.metadata.accounting_errors == 0U &&
                     stats.retention.accounting_errors == 0U,
                 "cache ledgers must remain clean");
    return ok;
}

bool deterministic_lru_and_reader_lifetime() {
    const Bytes font = make_font();
    VerifiedFontResourceCache cache(font.size() * 2U, 64U * 1024U, 2U);
    std::shared_ptr<const VerifiedFontResource> first;
    std::shared_ptr<const VerifiedFontResource> second;
    std::shared_ptr<const VerifiedFontResource> third;
    VerifiedFontResourceCacheStats stats;
    VerifiedFontResourceCacheError error;

    bool ok = true;
    ok &= expect(cache.get_or_build(key(2U, 1U), font, &first, &stats, &error),
                 "first LRU entry must build");
    ok &= expect(cache.get_or_build(key(2U, 2U), font, &second, &stats, &error),
                 "second LRU entry must build");
    std::shared_ptr<const VerifiedFontResource> touch;
    ok &= expect(cache.lookup(key(2U, 1U), &touch, &stats, &error),
                 "first entry must be touched before eviction");
    ok &= expect(cache.get_or_build(key(2U, 3U), font, &third, &stats, &error),
                 "third entry must trigger bounded eviction");
    ok &= expect(stats.entry_count == 2U && stats.evictions == 1U &&
                     stats.retention.current_bytes == font.size() * 2U,
                 "cache must retain exactly two fonts after eviction");

    std::shared_ptr<const VerifiedFontResource> missing;
    ok &= expect(!cache.lookup(key(2U, 2U), &missing, &stats, &error) &&
                     error.kind == VerifiedFontResourceCacheErrorKind::CacheMiss,
                 "least-recently-used entry must be evicted");
    ok &= expect(cache.lookup(key(2U, 1U), &touch, &stats, &error),
                 "recently touched entry must remain resident");
    ok &= expect(cache.lookup(key(2U, 3U), &touch, &stats, &error),
                 "new entry must remain resident");
    ok &= expect(second && second->view().valid() &&
                     second->bytes().size() == font.size(),
                 "evicted external reader must remain valid");
    ok &= expect(first->resource_id() != second->resource_id() &&
                     second->resource_id() != third->resource_id(),
                 "separate cache publications require unique resource IDs");
    return ok;
}

bool concurrent_single_flight() {
    constexpr std::size_t kThreads = 12U;
    const Bytes font = make_font();
    VerifiedFontResourceCache cache(font.size() * 2U, 128U * 1024U, 2U);
    std::array<std::shared_ptr<const VerifiedFontResource>, kThreads> outputs;
    std::array<bool, kThreads> results{};
    std::array<std::thread, kThreads> threads;
    std::atomic<bool> start{false};

    for (std::size_t index = 0U; index < kThreads; ++index) {
        threads[index] = std::thread([&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            VerifiedFontResourceCacheStats local_stats;
            VerifiedFontResourceCacheError local_error;
            results[index] = cache.get_or_build(
                key(3U, 1U),
                font,
                &outputs[index],
                &local_stats,
                &local_error);
        });
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }

    bool ok = true;
    for (std::size_t index = 0U; index < kThreads; ++index) {
        ok &= expect(results[index] && outputs[index],
                     "every concurrent acquire must succeed");
        if (index != 0U) {
            ok &= expect(outputs[index] == outputs[0U],
                         "all concurrent readers must share one handle");
        }
    }
    const VerifiedFontResourceCacheStats stats = cache.snapshot();
    ok &= expect(stats.build_attempts == 1U &&
                     stats.builds_published == 1U &&
                     stats.entry_count == 1U &&
                     stats.inflight_count == 0U,
                 "same-key concurrent misses must be single-flight");
    ok &= expect(stats.hits + 1U == kThreads,
                 "all non-building concurrent acquires must become hits");
    return ok;
}

bool budgets_clear_and_arguments() {
    const Bytes font = make_font();
    std::shared_ptr<const VerifiedFontResource> output;
    VerifiedFontResourceCacheStats stats;
    VerifiedFontResourceCacheError error;

    VerifiedFontResourceCache too_small(font.size() - 1U, 64U * 1024U, 2U);
    bool ok = expect(
        !too_small.get_or_build(key(4U, 1U), font, &output, &stats, &error),
        "source above cache retention limit must fail before build");
    ok &= expect(!output &&
                     error.kind ==
                         VerifiedFontResourceCacheErrorKind::SourceExceedsRetentionLimit &&
                     stats.build_attempts == 0U,
                 "retention rejection must avoid resource construction");

    VerifiedFontResourceCache no_metadata(font.size() * 2U, 1U, 2U);
    ok &= expect(
        !no_metadata.get_or_build(
            key(4U, 2U), font, &output, &stats, &error),
        "one-byte metadata budget must reject in-flight state");
    ok &= expect(!output &&
                     error.kind ==
                         VerifiedFontResourceCacheErrorKind::MetadataBudgetExceeded &&
                     stats.metadata.rejected_reservations != 0U,
                 "metadata rejection must be ledger-visible");

    VerifiedFontResourceCache cache(font.size() * 2U, 64U * 1024U, 2U);
    ok &= expect(
        !cache.lookup({}, &output, &stats, &error) &&
            error.kind == VerifiedFontResourceCacheErrorKind::InvalidArgument,
        "zero cache key must fail");
    ok &= expect(
        !cache.get_or_build(key(4U, 3U), {}, &output, &stats, &error) &&
            error.kind == VerifiedFontResourceCacheErrorKind::InvalidArgument,
        "build request requires source bytes");

    ok &= expect(cache.get_or_build(key(4U, 4U), font, &output, &stats, &error),
                 "clear fixture must build");
    std::shared_ptr<const VerifiedFontResource> retained = output;
    cache.clear();
    stats = cache.snapshot();
    ok &= expect(stats.entry_count == 0U &&
                     stats.retention.current_bytes == 0U &&
                     stats.clears == 1U,
                 "clear must release all cache-owned retention");
    ok &= expect(retained && retained->view().valid(),
                 "clear must not invalidate external readers");
    ok &= expect(!cache.lookup(key(4U, 4U), &output, &stats, &error) &&
                     error.kind == VerifiedFontResourceCacheErrorKind::CacheMiss,
                 "cleared entry must no longer be resident");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= hit_miss_and_collision();
    ok &= deterministic_lru_and_reader_lifetime();
    ok &= concurrent_single_flight();
    ok &= budgets_clear_and_arguments();
    if (!ok) {
        return 1;
    }
    std::cout << "verified-font-resource-cache-tests: PASS\n";
    return 0;
}
