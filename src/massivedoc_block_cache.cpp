#include "massivedoc_block_cache.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace zevryon::massivedoc {
namespace {

struct BlockKey {
    std::uint32_t source_id{0U};
    std::uint64_t block_index{0U};

    friend bool operator<(const BlockKey& left, const BlockKey& right) noexcept {
        if (left.source_id != right.source_id) {
            return left.source_id < right.source_id;
        }
        return left.block_index < right.block_index;
    }
};

struct BlockEntry {
    ImmutableBlockTier tier{ImmutableBlockTier::warm};
    std::uint64_t touch{0U};
    std::vector<std::byte> bytes;
};

} // namespace

bool validate_immutable_block_cache_config(
    ImmutableBlockCacheConfig config,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (config.block_bytes == 0U ||
        config.block_bytes > kMaximumImmutableBlockBytes) {
        *error = "immutable block size is outside the supported bounded range";
        return false;
    }
    if (config.hot_bytes > kMaximumResidentBlockCacheBytes ||
        config.warm_bytes > kMaximumResidentBlockCacheBytes ||
        config.hot_bytes > kMaximumResidentBlockCacheBytes - config.warm_bytes) {
        *error = "immutable block cache resident budget exceeds the supported hard limit";
        return false;
    }
    if (config.hot_bytes != 0U && config.hot_bytes < config.block_bytes) {
        *error = "hot immutable block cache budget cannot hold one full block";
        return false;
    }
    if (config.warm_bytes != 0U && config.warm_bytes < config.block_bytes) {
        *error = "warm immutable block cache budget cannot hold one full block";
        return false;
    }
    if (config.hot_bytes != 0U && config.warm_bytes == 0U) {
        *error = "hot immutable block cache requires a warm admission tier";
        return false;
    }
    return true;
}

struct ImmutableBlockCache::Impl {
    explicit Impl(ImmutableBlockCacheConfig cache_config)
        : config(cache_config) {
        const std::size_t resident_limit = config.hot_bytes + config.warm_bytes;
        ledger.set_hard_limit(core::ResourceClass::SourceWindow, resident_limit);
    }

    void touch(BlockEntry* entry) noexcept {
        if (touch_counter == std::numeric_limits<std::uint64_t>::max()) {
            std::uint64_t next = 1U;
            for (auto& item : entries) {
                item.second.touch = next;
                if (next != std::numeric_limits<std::uint64_t>::max()) {
                    ++next;
                }
            }
            touch_counter = next;
        }
        entry->touch = touch_counter;
        if (touch_counter != std::numeric_limits<std::uint64_t>::max()) {
            ++touch_counter;
        }
    }

    auto oldest(ImmutableBlockTier tier, const std::optional<BlockKey>& excluded)
        -> std::map<BlockKey, BlockEntry>::iterator {
        auto selected = entries.end();
        for (auto iterator = entries.begin(); iterator != entries.end(); ++iterator) {
            if (iterator->second.tier != tier ||
                (excluded && !(iterator->first < *excluded) && !(*excluded < iterator->first))) {
                continue;
            }
            if (selected == entries.end() || iterator->second.touch < selected->second.touch) {
                selected = iterator;
            }
        }
        return selected;
    }

    bool evict_oldest_warm(const std::optional<BlockKey>& excluded) noexcept {
        auto iterator = oldest(ImmutableBlockTier::warm, excluded);
        if (iterator == entries.end()) {
            return false;
        }
        const std::size_t bytes = iterator->second.bytes.size();
        warm_resident_bytes -= std::min(warm_resident_bytes, bytes);
        ledger.release(core::ResourceClass::SourceWindow, bytes);
        ledger.record_eviction(core::ResourceClass::SourceWindow);
        ++evictions;
        entries.erase(iterator);
        return true;
    }

    bool demote_oldest_hot(const std::optional<BlockKey>& excluded) noexcept {
        auto iterator = oldest(ImmutableBlockTier::hot, excluded);
        if (iterator == entries.end()) {
            return false;
        }
        const std::size_t bytes = iterator->second.bytes.size();
        hot_resident_bytes -= std::min(hot_resident_bytes, bytes);
        warm_resident_bytes += bytes;
        iterator->second.tier = ImmutableBlockTier::warm;
        ++demotions;
        while (warm_resident_bytes > config.warm_bytes) {
            if (!evict_oldest_warm(excluded)) {
                return false;
            }
        }
        return true;
    }

    void promote(const BlockKey& key) noexcept {
        auto iterator = entries.find(key);
        if (iterator == entries.end() ||
            iterator->second.tier != ImmutableBlockTier::warm ||
            config.hot_bytes == 0U) {
            return;
        }
        const std::size_t bytes = iterator->second.bytes.size();
        warm_resident_bytes -= std::min(warm_resident_bytes, bytes);
        while (hot_resident_bytes > config.hot_bytes - bytes) {
            if (!demote_oldest_hot(key)) {
                warm_resident_bytes += bytes;
                return;
            }
        }
        iterator = entries.find(key);
        if (iterator == entries.end()) {
            return;
        }
        iterator->second.tier = ImmutableBlockTier::hot;
        hot_resident_bytes += bytes;
        ++promotions;
    }

    BlockEntry* load_cold(
        const BlockKey& key,
        std::uint64_t block_offset,
        const ImmutableBlockLoader& loader,
        std::vector<std::byte>* direct,
        std::string* error) {
        ledger.record_cache_miss(core::ResourceClass::SourceWindow);
        ++cold_misses;
        direct->clear();
        if (!loader(key.source_id, block_offset, config.block_bytes, direct, error)) {
            return nullptr;
        }
        if (direct->empty() || direct->size() > config.block_bytes) {
            *error = "immutable block loader returned an invalid block length";
            return nullptr;
        }
        ledger.record_physical_read(
            core::ResourceClass::SourceWindow,
            static_cast<std::uint64_t>(direct->size()));

        if (config.warm_bytes == 0U || direct->size() > config.warm_bytes) {
            return nullptr;
        }
        while (warm_resident_bytes > config.warm_bytes - direct->size()) {
            if (!evict_oldest_warm(std::nullopt)) {
                return nullptr;
            }
        }
        if (!ledger.try_reserve(core::ResourceClass::SourceWindow, direct->size())) {
            return nullptr;
        }

        BlockEntry entry;
        entry.tier = ImmutableBlockTier::warm;
        entry.bytes = std::move(*direct);
        touch(&entry);
        const std::size_t bytes = entry.bytes.size();
        auto [iterator, inserted] = entries.emplace(key, std::move(entry));
        if (!inserted) {
            ledger.release(core::ResourceClass::SourceWindow, bytes);
            return &iterator->second;
        }
        warm_resident_bytes += bytes;
        peak_resident_bytes = std::max(
            peak_resident_bytes,
            hot_resident_bytes + warm_resident_bytes);
        return &iterator->second;
    }

    ImmutableBlockCacheConfig config;
    mutable std::mutex mutex;
    std::map<BlockKey, BlockEntry> entries;
    core::ResourceLedger ledger;
    std::size_t hot_resident_bytes{0U};
    std::size_t warm_resident_bytes{0U};
    std::size_t peak_resident_bytes{0U};
    std::uint64_t touch_counter{1U};
    std::uint64_t hot_hits{0U};
    std::uint64_t warm_hits{0U};
    std::uint64_t cold_misses{0U};
    std::uint64_t promotions{0U};
    std::uint64_t demotions{0U};
    std::uint64_t evictions{0U};
};

ImmutableBlockCache::ImmutableBlockCache(ImmutableBlockCacheConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

ImmutableBlockCache::~ImmutableBlockCache() = default;

bool ImmutableBlockCache::read_exact(
    std::uint32_t source_id,
    std::uint64_t offset,
    std::span<std::byte> destination,
    const ImmutableBlockLoader& loader,
    std::string* error) {
    if (error == nullptr || !loader) {
        if (error != nullptr) {
            *error = "invalid immutable block cache read request";
        }
        return false;
    }
    error->clear();
    std::string config_error;
    if (!validate_immutable_block_cache_config(impl_->config, &config_error)) {
        *error = config_error;
        return false;
    }
    if (destination.empty()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::size_t copied = 0U;
    std::vector<std::byte> direct;
    while (copied < destination.size()) {
        const std::uint64_t block_bytes = static_cast<std::uint64_t>(impl_->config.block_bytes);
        const std::uint64_t block_index = offset / block_bytes;
        if (block_index > std::numeric_limits<std::uint64_t>::max() / block_bytes) {
            *error = "immutable block cache offset overflow";
            return false;
        }
        const std::uint64_t block_offset = block_index * block_bytes;
        const BlockKey key{source_id, block_index};
        auto iterator = impl_->entries.find(key);
        const std::vector<std::byte>* bytes = nullptr;
        if (iterator != impl_->entries.end()) {
            impl_->ledger.record_cache_hit(core::ResourceClass::SourceWindow);
            if (iterator->second.tier == ImmutableBlockTier::hot) {
                ++impl_->hot_hits;
                impl_->touch(&iterator->second);
            } else {
                ++impl_->warm_hits;
                impl_->touch(&iterator->second);
                impl_->promote(key);
                iterator = impl_->entries.find(key);
                if (iterator == impl_->entries.end()) {
                    *error = "immutable block cache promotion lost the requested block";
                    return false;
                }
            }
            bytes = &iterator->second.bytes;
        } else {
            BlockEntry* admitted = impl_->load_cold(
                key,
                block_offset,
                loader,
                &direct,
                error);
            if (!error->empty()) {
                return false;
            }
            bytes = admitted != nullptr ? &admitted->bytes : &direct;
        }

        if (offset < block_offset) {
            *error = "immutable block cache offset accounting underflow";
            return false;
        }
        const std::uint64_t offset64 = offset - block_offset;
        if (offset64 >= static_cast<std::uint64_t>(bytes->size())) {
            *error = "immutable block cache read extends beyond source bytes";
            return false;
        }
        const std::size_t offset_in_block = static_cast<std::size_t>(offset64);
        const std::size_t available = bytes->size() - offset_in_block;
        const std::size_t amount = std::min(destination.size() - copied, available);
        std::memcpy(destination.data() + copied, bytes->data() + offset_in_block, amount);
        copied += amount;
        if (offset > std::numeric_limits<std::uint64_t>::max() - amount) {
            *error = "immutable block cache read offset overflow";
            return false;
        }
        offset += static_cast<std::uint64_t>(amount);
    }
    return true;
}

void ImmutableBlockCache::evict_all_to_cold() noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& item : impl_->entries) {
        impl_->ledger.release(core::ResourceClass::SourceWindow, item.second.bytes.size());
        impl_->ledger.record_eviction(core::ResourceClass::SourceWindow);
        ++impl_->evictions;
    }
    impl_->entries.clear();
    impl_->hot_resident_bytes = 0U;
    impl_->warm_resident_bytes = 0U;
}

ImmutableBlockCacheStats ImmutableBlockCache::stats() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ImmutableBlockCacheStats snapshot;
    snapshot.hot_resident_bytes = impl_->hot_resident_bytes;
    snapshot.warm_resident_bytes = impl_->warm_resident_bytes;
    snapshot.resident_bytes = impl_->hot_resident_bytes + impl_->warm_resident_bytes;
    snapshot.peak_resident_bytes = impl_->peak_resident_bytes;
    for (const auto& item : impl_->entries) {
        if (item.second.tier == ImmutableBlockTier::hot) {
            ++snapshot.hot_blocks;
        } else if (item.second.tier == ImmutableBlockTier::warm) {
            ++snapshot.warm_blocks;
        }
    }
    snapshot.hot_hits = impl_->hot_hits;
    snapshot.warm_hits = impl_->warm_hits;
    snapshot.cold_misses = impl_->cold_misses;
    snapshot.promotions = impl_->promotions;
    snapshot.demotions = impl_->demotions;
    snapshot.evictions = impl_->evictions;
    const auto ledger_snapshot = impl_->ledger.snapshot(core::ResourceClass::SourceWindow);
    snapshot.physical_read_bytes = ledger_snapshot.physical_read_bytes;
    snapshot.ledger_within_hard_limits = impl_->ledger.within_hard_limits();
    snapshot.ledger_accounting_clean = impl_->ledger.accounting_clean();
    return snapshot;
}

ImmutableBlockCacheConfig ImmutableBlockCache::config() const noexcept {
    return impl_->config;
}

} // namespace zevryon::massivedoc
