#include "zenith_hot_scroll.hpp"

#include "compact_document.hpp"
#include "layout_checkpoint.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <list>
#include <sstream>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

struct ScrollAnchor {
    bool present{false};
    std::uint64_t record_index{0};
    std::uint64_t old_y_q8{0};
    std::uint64_t old_height_q8{0};
    std::uint64_t local_q8{0};
    std::uint64_t new_y_q8{0};
    std::uint64_t new_height_q8{0};
};

struct CheckpointKey {
    std::uint64_t record_index{0};
    std::uint32_t width_q8{0};

    bool operator==(const CheckpointKey&) const noexcept = default;
};

struct CheckpointKeyHash {
    std::size_t operator()(const CheckpointKey& key) const noexcept {
        std::size_t value = static_cast<std::size_t>(
            key.record_index ^ (key.record_index >> 32U));
        value ^= static_cast<std::size_t>(key.width_q8) +
                 static_cast<std::size_t>(0x9e3779b9U) + (value << 6U) +
                 (value >> 2U);
        return value;
    }
};

struct SourceWindowKey {
    std::uint64_t record_index{0};
    std::uint64_t source_offset{0};
    std::size_t request_bytes{0};

    bool operator==(const SourceWindowKey&) const noexcept = default;
};

struct SourceWindowKeyHash {
    std::size_t operator()(const SourceWindowKey& key) const noexcept {
        std::size_t value = static_cast<std::size_t>(
            key.record_index ^ (key.record_index >> 32U));
        const auto mix = [&value](std::uint64_t part) {
            const std::size_t folded = static_cast<std::size_t>(part ^ (part >> 32U));
            value ^= folded + static_cast<std::size_t>(0x9e3779b9U) +
                     (value << 6U) + (value >> 2U);
        };
        mix(key.source_offset);
        mix(static_cast<std::uint64_t>(key.request_bytes));
        return value;
    }
};

struct CheckpointCacheEntry {
    LayoutCheckpointIndex index;
    std::size_t charge_bytes{0};
    std::list<CheckpointKey>::iterator lru_position;
};

struct SourceWindowCacheEntry {
    std::vector<std::byte> bytes;
    std::size_t charge_bytes{0};
    std::list<SourceWindowKey>::iterator lru_position;
};

constexpr std::size_t kCacheContainerPointerSlots = 12U;
constexpr std::size_t kCheckpointCacheFixedCharge =
    sizeof(LayoutCheckpointIndex) + sizeof(CheckpointKey) +
    sizeof(std::list<CheckpointKey>::iterator) +
    kCacheContainerPointerSlots * sizeof(void*);
constexpr std::size_t kSourceWindowCacheFixedCharge =
    sizeof(std::vector<std::byte>) + sizeof(SourceWindowKey) +
    sizeof(std::list<SourceWindowKey>::iterator) +
    kCacheContainerPointerSlots * sizeof(void*);

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

std::uint64_t saturating_multiply(std::uint64_t left, std::uint64_t right) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

std::size_t saturating_size_add(std::size_t left, std::size_t right) noexcept {
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        return std::numeric_limits<std::size_t>::max();
    }
    return left + right;
}

bool intersects(
    std::uint64_t start,
    std::uint64_t height,
    std::uint64_t query_start,
    std::uint64_t query_end) noexcept {
    return saturating_add(start, height) > query_start && start < query_end;
}

std::uint32_t bucket_width(std::uint32_t width_q8, std::uint32_t bucket_q8) noexcept {
    if (bucket_q8 == 0U || width_q8 < bucket_q8) {
        return width_q8;
    }
    return static_cast<std::uint32_t>((width_q8 / bucket_q8) * bucket_q8);
}

LayoutCheckpointConfig checkpoint_config_for(
    const LayoutConfig& config,
    std::uint32_t viewport_width_q8) noexcept {
    LayoutCheckpointConfig checkpoint;
    checkpoint.width_q8 = bucket_width(viewport_width_q8, config.width_bucket_q8);
    checkpoint.average_advance_q8 = config.average_advance_q8;
    checkpoint.line_height_q8 = config.line_height_q8;
    checkpoint.horizontal_padding_q8 = config.horizontal_padding_q8;
    checkpoint.vertical_padding_q8 = config.vertical_padding_q8;
    checkpoint.target_stride_bytes = config.checkpoint_stride_bytes;
    return checkpoint;
}

ScrollAnchor select_scroll_anchor(
    const ViewportResult& viewport,
    std::uint64_t scroll_y_q8) noexcept {
    ScrollAnchor anchor;
    for (const MaterializedRecord& record : viewport.records) {
        const std::uint64_t end_q8 = saturating_add(record.y_q8, record.height_q8);
        if (scroll_y_q8 >= record.y_q8 && scroll_y_q8 < end_q8) {
            anchor.present = true;
            anchor.record_index = record.record_index;
            anchor.old_y_q8 = record.y_q8;
            anchor.old_height_q8 = record.height_q8;
            anchor.local_q8 = scroll_y_q8 - record.y_q8;
            anchor.new_y_q8 = record.y_q8;
            anchor.new_height_q8 = record.height_q8;
            return anchor;
        }
    }
    if (viewport.records.empty()) {
        return anchor;
    }
    const MaterializedRecord& fallback = scroll_y_q8 < viewport.records.front().y_q8
                                             ? viewport.records.front()
                                             : viewport.records.back();
    anchor.present = true;
    anchor.record_index = fallback.record_index;
    anchor.old_y_q8 = fallback.y_q8;
    anchor.old_height_q8 = fallback.height_q8;
    anchor.local_q8 = scroll_y_q8 > fallback.y_q8 ? scroll_y_q8 - fallback.y_q8 : 0U;
    if (anchor.old_height_q8 != 0U && anchor.local_q8 >= anchor.old_height_q8) {
        anchor.local_q8 = anchor.old_height_q8 - 1U;
    }
    anchor.new_y_q8 = fallback.y_q8;
    anchor.new_height_q8 = fallback.height_q8;
    return anchor;
}

void apply_height_to_anchor(
    ScrollAnchor* anchor,
    const MaterializedRecord& record,
    std::uint32_t measured_height_q8) noexcept {
    if (anchor == nullptr || !anchor->present) {
        return;
    }
    const std::uint64_t old_height_q8 = record.height_q8;
    const std::uint64_t new_height_q8 = measured_height_q8;
    if (record.record_index < anchor->record_index) {
        if (new_height_q8 >= old_height_q8) {
            anchor->new_y_q8 = saturating_add(
                anchor->new_y_q8, new_height_q8 - old_height_q8);
        } else {
            const std::uint64_t delta = old_height_q8 - new_height_q8;
            anchor->new_y_q8 = anchor->new_y_q8 >= delta
                                   ? anchor->new_y_q8 - delta
                                   : 0U;
        }
    } else if (record.record_index == anchor->record_index) {
        anchor->new_height_q8 = new_height_q8;
    }
}

std::uint64_t anchored_scroll(
    const ScrollAnchor& anchor,
    std::uint64_t original_scroll_q8) noexcept {
    if (!anchor.present || anchor.old_height_q8 == 0U || anchor.new_height_q8 == 0U) {
        return original_scroll_q8;
    }
    const std::uint64_t scaled_local_q8 =
        (anchor.local_q8 * anchor.new_height_q8) / anchor.old_height_q8;
    return saturating_add(
        anchor.new_y_q8,
        std::min(scaled_local_q8, anchor.new_height_q8 - 1U));
}

std::size_t checkpoint_cache_charge(const LayoutCheckpointIndex& index) noexcept {
    const std::size_t capacity = index.entries().capacity();
    if (capacity >
        (std::numeric_limits<std::size_t>::max() - kCheckpointCacheFixedCharge) /
            sizeof(LayoutCheckpointEntry)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return kCheckpointCacheFixedCharge + capacity * sizeof(LayoutCheckpointEntry);
}

std::size_t source_window_cache_charge(const std::vector<std::byte>& bytes) noexcept {
    if (bytes.capacity() >
        std::numeric_limits<std::size_t>::max() - kSourceWindowCacheFixedCharge) {
        return std::numeric_limits<std::size_t>::max();
    }
    return kSourceWindowCacheFixedCharge + bytes.capacity();
}

} // namespace

struct ZenithHotScrollSession::Impl {
    explicit Impl(const std::filesystem::path& root_value, LayoutConfig config_value)
        : root(root_value),
          config(config_value),
          store(root_value),
          arena(root_value) {}

    std::filesystem::path root;
    LayoutConfig config;
    StoreReader store;
    CompactArenaReader arena;
    bool opened{false};
    ZenithHotScrollStats statistics;

    std::list<CheckpointKey> checkpoint_lru;
    std::unordered_map<CheckpointKey, CheckpointCacheEntry, CheckpointKeyHash>
        checkpoint_cache;
    std::list<SourceWindowKey> source_lru;
    std::unordered_map<SourceWindowKey, SourceWindowCacheEntry, SourceWindowKeyHash>
        source_cache;
    std::vector<std::byte> source_scratch;
    std::vector<LayoutFragment> fragment_scratch;

    void erase_checkpoint_cache(
        std::unordered_map<CheckpointKey, CheckpointCacheEntry, CheckpointKeyHash>::iterator
            entry) {
        if (entry->second.charge_bytes <= statistics.checkpoint_cache_bytes) {
            statistics.checkpoint_cache_bytes -= entry->second.charge_bytes;
        } else {
            statistics.checkpoint_cache_bytes = 0U;
        }
        checkpoint_lru.erase(entry->second.lru_position);
        checkpoint_cache.erase(entry);
    }

    void evict_checkpoint_for(std::size_t incoming) {
        while (!checkpoint_lru.empty() &&
               (incoming > config.max_checkpoint_cache_bytes ||
                statistics.checkpoint_cache_bytes >
                    config.max_checkpoint_cache_bytes - incoming)) {
            const CheckpointKey key = checkpoint_lru.back();
            const auto found = checkpoint_cache.find(key);
            if (found == checkpoint_cache.end()) {
                checkpoint_lru.pop_back();
                continue;
            }
            erase_checkpoint_cache(found);
            ++statistics.checkpoint_cache_evictions;
        }
    }

    void erase_source_cache(
        std::unordered_map<SourceWindowKey, SourceWindowCacheEntry, SourceWindowKeyHash>::iterator
            entry) {
        if (entry->second.charge_bytes <= statistics.source_window_cache_bytes) {
            statistics.source_window_cache_bytes -= entry->second.charge_bytes;
        } else {
            statistics.source_window_cache_bytes = 0U;
        }
        source_lru.erase(entry->second.lru_position);
        source_cache.erase(entry);
    }

    void evict_source_for(std::size_t incoming) {
        while (!source_lru.empty() &&
               (incoming > config.max_source_window_cache_bytes ||
                statistics.source_window_cache_bytes >
                    config.max_source_window_cache_bytes - incoming)) {
            const SourceWindowKey key = source_lru.back();
            const auto found = source_cache.find(key);
            if (found == source_cache.end()) {
                source_lru.pop_back();
                continue;
            }
            erase_source_cache(found);
            ++statistics.source_window_cache_evictions;
        }
    }

    bool acquire_checkpoint(
        const MaterializedRecord& record,
        const LayoutCheckpointConfig& checkpoint_config,
        LayoutCheckpointIndex* scratch,
        const LayoutCheckpointIndex** checkpoint,
        bool* available,
        std::string* error) {
        if (scratch == nullptr || checkpoint == nullptr || available == nullptr ||
            error == nullptr) {
            return false;
        }
        *checkpoint = nullptr;
        *available = false;
        if (!config.enable_persistent_checkpoints ||
            record.source_bytes < config.checkpoint_min_record_bytes) {
            return true;
        }

        const CheckpointKey key{record.record_index, checkpoint_config.width_q8};
        auto found = checkpoint_cache.find(key);
        if (found != checkpoint_cache.end()) {
            const LayoutCheckpointStats& cached = found->second.index.stats();
            if (cached.logical_id == record.logical_id &&
                cached.source_bytes == record.source_bytes) {
                checkpoint_lru.splice(
                    checkpoint_lru.begin(), checkpoint_lru, found->second.lru_position);
                found->second.lru_position = checkpoint_lru.begin();
                ++statistics.checkpoint_cache_hits;
                *checkpoint = &found->second.index;
                *available = true;
                return true;
            }
            erase_checkpoint_cache(found);
        }

        ++statistics.checkpoint_cache_misses;
        std::error_code exists_error;
        const bool exists = std::filesystem::exists(
            layout_checkpoint_path(root, record.record_index, checkpoint_config),
            exists_error);
        if (exists_error || !exists) {
            error->clear();
            return true;
        }

        LayoutCheckpointIndex loaded;
        if (!loaded.open(
                root,
                record.record_index,
                record.logical_id,
                record.source_bytes,
                checkpoint_config,
                error)) {
            error->clear();
            return true;
        }

        const std::size_t charge = checkpoint_cache_charge(loaded);
        if (charge <= config.max_checkpoint_cache_bytes) {
            evict_checkpoint_for(charge);
            if (statistics.checkpoint_cache_bytes <=
                config.max_checkpoint_cache_bytes - charge) {
                checkpoint_lru.push_front(key);
                CheckpointCacheEntry entry;
                entry.index = std::move(loaded);
                entry.charge_bytes = charge;
                entry.lru_position = checkpoint_lru.begin();
                const auto inserted = checkpoint_cache.emplace(key, std::move(entry));
                if (inserted.second) {
                    statistics.checkpoint_cache_bytes += charge;
                    statistics.checkpoint_cache_peak_bytes = std::max(
                        statistics.checkpoint_cache_peak_bytes,
                        statistics.checkpoint_cache_bytes);
                    *checkpoint = &inserted.first->second.index;
                    *available = true;
                    return true;
                }
                checkpoint_lru.pop_front();
            }
        }

        *scratch = std::move(loaded);
        *checkpoint = scratch;
        *available = true;
        return true;
    }

    bool read_source_window(
        const MaterializedRecord& record,
        std::uint64_t source_offset,
        std::size_t request_bytes,
        std::span<const std::byte>* bytes,
        std::uint64_t* physical_bytes_read,
        std::string* error) {
        if (bytes == nullptr || physical_bytes_read == nullptr || error == nullptr ||
            request_bytes == 0U) {
            return false;
        }
        *physical_bytes_read = 0U;
        const SourceWindowKey key{record.record_index, source_offset, request_bytes};
        auto found = source_cache.find(key);
        if (found != source_cache.end()) {
            source_lru.splice(source_lru.begin(), source_lru, found->second.lru_position);
            found->second.lru_position = source_lru.begin();
            ++statistics.source_window_cache_hits;
            *bytes = std::span<const std::byte>(found->second.bytes);
            return true;
        }

        ++statistics.source_window_cache_misses;
        if (!store.read_record_slice(
                record.record_index,
                source_offset,
                request_bytes,
                &source_scratch,
                error)) {
            return false;
        }
        *physical_bytes_read = static_cast<std::uint64_t>(source_scratch.size());

        const std::size_t charge = source_window_cache_charge(source_scratch);
        if (charge <= config.max_source_window_cache_bytes) {
            evict_source_for(charge);
            if (statistics.source_window_cache_bytes <=
                config.max_source_window_cache_bytes - charge) {
                source_lru.push_front(key);
                SourceWindowCacheEntry entry;
                entry.bytes = source_scratch;
                entry.charge_bytes = charge;
                entry.lru_position = source_lru.begin();
                const auto inserted = source_cache.emplace(key, std::move(entry));
                if (inserted.second) {
                    statistics.source_window_cache_bytes += charge;
                    statistics.source_window_cache_peak_bytes = std::max(
                        statistics.source_window_cache_peak_bytes,
                        statistics.source_window_cache_bytes);
                    *bytes = std::span<const std::byte>(inserted.first->second.bytes);
                    return true;
                }
                source_lru.pop_front();
            }
        }

        *bytes = std::span<const std::byte>(source_scratch);
        return true;
    }

    bool scan_record_window(
        const MaterializedRecord& record,
        const LayoutCheckpointIndex& checkpoint,
        std::uint64_t visible_start_q8,
        std::uint64_t visible_end_q8,
        std::size_t max_fragments,
        std::vector<LayoutFragment>* fragments,
        std::uint64_t* source_bytes_read,
        bool* truncated,
        std::string* error) {
        if (fragments == nullptr || source_bytes_read == nullptr || truncated == nullptr ||
            error == nullptr || max_fragments == 0U ||
            visible_end_q8 <= visible_start_q8) {
            if (error != nullptr) {
                *error = "invalid hot-scroll checkpoint request";
            }
            return false;
        }
        error->clear();
        fragments->clear();
        *source_bytes_read = 0U;
        *truncated = false;

        const LayoutCheckpointStats& checkpoint_stats = checkpoint.stats();
        if (checkpoint_stats.record_index != record.record_index ||
            checkpoint_stats.logical_id != record.logical_id ||
            checkpoint_stats.source_bytes != record.source_bytes) {
            *error = "hot-scroll checkpoint does not match record";
            return false;
        }
        const LayoutCheckpointConfig& checkpoint_config = checkpoint_stats.config;
        const LayoutCheckpointEntry& start_entry = checkpoint.entry_for_y(visible_start_q8);
        const std::uint64_t horizontal =
            static_cast<std::uint64_t>(checkpoint_config.horizontal_padding_q8) * 2U;
        const std::uint64_t content_width = checkpoint_config.width_q8 > horizontal
                                                ? static_cast<std::uint64_t>(checkpoint_config.width_q8) - horizontal
                                                : static_cast<std::uint64_t>(checkpoint_config.average_advance_q8);
        const std::uint64_t columns_per_line = std::max<std::uint64_t>(
            1U,
            content_width /
                std::max<std::uint64_t>(1U, checkpoint_config.average_advance_q8));
        const std::uint64_t top_padding = checkpoint_config.vertical_padding_q8 / 2U;

        std::uint64_t source_offset = start_entry.source_offset;
        std::uint64_t line_start = source_offset;
        std::uint64_t columns = 0U;
        std::uint64_t line_count = start_entry.line_index;
        std::uint8_t pending_remaining = 0U;
        bool stop = false;

        const auto emit_line = [&](std::uint64_t source_end, bool hard_break) {
            const std::uint64_t local_y = saturating_add(
                top_padding,
                saturating_multiply(line_count, checkpoint_config.line_height_q8));
            if (intersects(
                    local_y,
                    checkpoint_config.line_height_q8,
                    visible_start_q8,
                    visible_end_q8)) {
                if (fragments->size() < max_fragments) {
                    const std::uint64_t raw_width = saturating_multiply(
                        columns, checkpoint_config.average_advance_q8);
                    LayoutFragment fragment;
                    fragment.record_index = record.record_index;
                    fragment.logical_id = record.logical_id;
                    fragment.source_start = line_start;
                    fragment.source_end = source_end;
                    fragment.x_q8 = checkpoint_config.horizontal_padding_q8;
                    fragment.y_q8 = local_y;
                    fragment.width_q8 = static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(
                            std::max<std::uint64_t>(
                                checkpoint_config.average_advance_q8, raw_width),
                            std::min<std::uint64_t>(
                                content_width,
                                std::numeric_limits<std::uint32_t>::max())));
                    fragment.height_q8 = checkpoint_config.line_height_q8;
                    fragment.hard_break = hard_break;
                    fragments->push_back(fragment);
                } else {
                    *truncated = true;
                }
            }
            ++line_count;
            line_start = source_end;
            columns = 0U;
            if (local_y >= visible_end_q8 || fragments->size() >= max_fragments) {
                stop = true;
            }
        };

        const auto consume_codepoint = [&](std::uint64_t end, std::uint32_t value) {
            if (value == 0x0aU) {
                emit_line(end, true);
                return;
            }
            columns = saturating_add(columns, value == 0x09U ? 4U : 1U);
            if (columns >= columns_per_line) {
                emit_line(end, false);
            }
        };

        while (!stop && source_offset < record.source_bytes) {
            const std::uint64_t remaining = record.source_bytes - source_offset;
            const std::size_t request = static_cast<std::size_t>(
                std::min<std::uint64_t>(kIoWindowBytes, remaining));
            std::span<const std::byte> chunk;
            std::uint64_t physical_bytes = 0U;
            if (!read_source_window(
                    record,
                    source_offset,
                    request,
                    &chunk,
                    &physical_bytes,
                    error)) {
                return false;
            }
            if (chunk.empty()) {
                *error = "hot-scroll source window ended before record length";
                return false;
            }
            *source_bytes_read = saturating_add(*source_bytes_read, physical_bytes);
            for (const std::byte raw : chunk) {
                if (stop) {
                    break;
                }
                const std::uint8_t byte = static_cast<std::uint8_t>(
                    std::to_integer<unsigned int>(raw));
                bool retry = true;
                while (retry && !stop) {
                    retry = false;
                    if (pending_remaining != 0U) {
                        if ((byte & 0xc0U) == 0x80U) {
                            --pending_remaining;
                            ++source_offset;
                            if (pending_remaining == 0U) {
                                consume_codepoint(source_offset, 0xfffdU);
                            }
                            continue;
                        }
                        consume_codepoint(source_offset, 0xfffdU);
                        pending_remaining = 0U;
                        retry = true;
                        continue;
                    }
                    ++source_offset;
                    if (byte < 0x80U) {
                        consume_codepoint(source_offset, byte);
                    } else if ((byte & 0xe0U) == 0xc0U) {
                        pending_remaining = 1U;
                    } else if ((byte & 0xf0U) == 0xe0U) {
                        pending_remaining = 2U;
                    } else if ((byte & 0xf8U) == 0xf0U) {
                        pending_remaining = 3U;
                    } else {
                        consume_codepoint(source_offset, 0xfffdU);
                    }
                }
            }
        }
        if (!stop && pending_remaining != 0U) {
            consume_codepoint(source_offset, 0xfffdU);
        }
        if (!stop && (line_start < source_offset || fragments->empty())) {
            emit_line(source_offset, false);
        }
        return true;
    }

    void publish_cache_metrics(
        const ZenithHotScrollStats& before,
        LayoutWindowResult* result) const noexcept {
        result->checkpoint_cache_hits =
            statistics.checkpoint_cache_hits - before.checkpoint_cache_hits;
        result->checkpoint_cache_misses =
            statistics.checkpoint_cache_misses - before.checkpoint_cache_misses;
        result->checkpoint_cache_evictions =
            statistics.checkpoint_cache_evictions - before.checkpoint_cache_evictions;
        result->source_window_cache_hits =
            statistics.source_window_cache_hits - before.source_window_cache_hits;
        result->source_window_cache_misses =
            statistics.source_window_cache_misses - before.source_window_cache_misses;
        result->source_window_cache_evictions =
            statistics.source_window_cache_evictions - before.source_window_cache_evictions;
        result->checkpoint_cache_bytes = statistics.checkpoint_cache_bytes;
        result->source_window_cache_bytes = statistics.source_window_cache_bytes;
        result->cache_bytes = saturating_size_add(
            statistics.checkpoint_cache_bytes,
            statistics.source_window_cache_bytes);
    }
};

ZenithHotScrollSession::ZenithHotScrollSession(
    const std::filesystem::path& store_root,
    LayoutConfig config)
    : impl_(std::make_unique<Impl>(store_root, config)) {}

ZenithHotScrollSession::~ZenithHotScrollSession() = default;

bool ZenithHotScrollSession::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->config.average_advance_q8 == 0U ||
        impl_->config.line_height_q8 == 0U ||
        impl_->config.width_bucket_q8 == 0U ||
        impl_->config.checkpoint_stride_bytes == 0U) {
        *error = "invalid hot-scroll layout configuration";
        return false;
    }
    if (!impl_->store.open(error) || !impl_->arena.open(error)) {
        return false;
    }
    impl_->opened = true;
    return true;
}

bool ZenithHotScrollSession::layout(
    std::uint64_t scroll_y_q8,
    std::uint32_t viewport_width_q8,
    std::uint64_t viewport_height_q8,
    std::uint64_t overscan_q8,
    std::size_t max_fragments,
    LayoutWindowResult* result,
    bool* used_checkpoint_path,
    std::string* error) {
    if (!impl_->opened || result == nullptr || used_checkpoint_path == nullptr ||
        error == nullptr || viewport_width_q8 == 0U || viewport_height_q8 == 0U ||
        max_fragments == 0U) {
        if (error != nullptr) {
            *error = "invalid hot-scroll layout request";
        }
        return false;
    }

    const ZenithHotScrollStats before = impl_->statistics;
    *result = LayoutWindowResult{};
    *used_checkpoint_path = false;
    error->clear();

    ViewportResult initial;
    if (!impl_->arena.materialize(
            scroll_y_q8,
            viewport_height_q8,
            overscan_q8,
            max_fragments,
            &initial,
            error)) {
        return false;
    }
    if (initial.records.empty()) {
        ++impl_->statistics.layout_calls;
        impl_->publish_cache_metrics(before, result);
        return true;
    }

    result->query_start_q8 = initial.query_start_q8;
    result->query_end_q8 = initial.query_end_q8;
    result->truncated = initial.truncated;
    ScrollAnchor anchor = select_scroll_anchor(initial, scroll_y_q8);
    const LayoutCheckpointConfig checkpoint_config =
        checkpoint_config_for(impl_->config, viewport_width_q8);
    std::unordered_set<std::uint64_t> charged_indices;

    for (const MaterializedRecord& record : initial.records) {
        LayoutCheckpointIndex scratch;
        const LayoutCheckpointIndex* checkpoint = nullptr;
        bool available = false;
        if (!impl_->acquire_checkpoint(
                record,
                checkpoint_config,
                &scratch,
                &checkpoint,
                &available,
                error)) {
            return false;
        }
        if (!available || checkpoint == nullptr) {
            *result = LayoutWindowResult{};
            ++impl_->statistics.layout_calls;
            impl_->publish_cache_metrics(before, result);
            return true;
        }
        const LayoutCheckpointStats& checkpoint_stats = checkpoint->stats();
        ++result->checkpoint_hits;
        ++result->measured_records;
        if (charged_indices.insert(record.record_index).second) {
            result->checkpoint_index_bytes = saturating_add(
                result->checkpoint_index_bytes, checkpoint_stats.physical_bytes);
        }
        result->height_saturated =
            result->height_saturated || checkpoint_stats.height_saturated;
        apply_height_to_anchor(&anchor, record, checkpoint_stats.measured_height_q8);
        if (record.height_q8 != checkpoint_stats.measured_height_q8) {
            HeightUpdateResult update;
            if (!impl_->arena.update_height(
                    record.record_index,
                    checkpoint_stats.measured_height_q8,
                    &update,
                    error)) {
                return false;
            }
        }
    }

    const std::uint64_t anchor_corrected_scroll = anchored_scroll(anchor, scroll_y_q8);
    result->scroll_anchor_adjusted = anchor_corrected_scroll != scroll_y_q8;
    const std::uint64_t corrected_total_height = impl_->arena.stats().total_height_q8;
    const std::uint64_t max_corrected_scroll =
        corrected_total_height > viewport_height_q8
            ? corrected_total_height - viewport_height_q8
            : 0U;
    const std::uint64_t corrected_scroll_y =
        std::min(anchor_corrected_scroll, max_corrected_scroll);
    result->scroll_clamped = corrected_scroll_y != anchor_corrected_scroll;

    ViewportResult corrected;
    if (!impl_->arena.materialize(
            corrected_scroll_y,
            viewport_height_q8,
            overscan_q8,
            max_fragments,
            &corrected,
            error)) {
        return false;
    }
    result->query_start_q8 = corrected.query_start_q8;
    result->query_end_q8 = corrected.query_end_q8;
    result->total_height_q8 = corrected.total_height_q8;
    result->truncated = result->truncated || corrected.truncated;

    for (const MaterializedRecord& record : corrected.records) {
        if (result->fragments.size() >= max_fragments) {
            result->truncated = true;
            break;
        }
        LayoutCheckpointIndex scratch;
        const LayoutCheckpointIndex* checkpoint = nullptr;
        bool available = false;
        if (!impl_->acquire_checkpoint(
                record,
                checkpoint_config,
                &scratch,
                &checkpoint,
                &available,
                error)) {
            return false;
        }
        if (!available || checkpoint == nullptr) {
            *result = LayoutWindowResult{};
            ++impl_->statistics.layout_calls;
            impl_->publish_cache_metrics(before, result);
            return true;
        }
        ++result->checkpoint_hits;
        if (charged_indices.insert(record.record_index).second) {
            result->checkpoint_index_bytes = saturating_add(
                result->checkpoint_index_bytes,
                checkpoint->stats().physical_bytes);
        }

        const std::uint64_t local_start = corrected.query_start_q8 > record.y_q8
                                              ? corrected.query_start_q8 - record.y_q8
                                              : 0U;
        const std::uint64_t local_end = corrected.query_end_q8 > record.y_q8
                                            ? corrected.query_end_q8 - record.y_q8
                                            : 0U;
        const std::size_t remaining = max_fragments - result->fragments.size();
        std::uint64_t physical_source_bytes = 0U;
        bool truncated = false;
        if (!impl_->scan_record_window(
                record,
                *checkpoint,
                local_start,
                local_end,
                remaining,
                &impl_->fragment_scratch,
                &physical_source_bytes,
                &truncated,
                error)) {
            return false;
        }
        result->source_bytes_read = saturating_add(
            result->source_bytes_read, physical_source_bytes);
        result->truncated = result->truncated || truncated;
        for (const LayoutFragment& local : impl_->fragment_scratch) {
            const std::uint64_t absolute_y = saturating_add(record.y_q8, local.y_q8);
            if (!intersects(
                    absolute_y,
                    local.height_q8,
                    corrected.query_start_q8,
                    corrected.query_end_q8)) {
                continue;
            }
            if (result->fragments.size() >= max_fragments) {
                result->truncated = true;
                break;
            }
            LayoutFragment absolute = local;
            absolute.y_q8 = absolute_y;
            result->fragments.push_back(absolute);
        }
    }

    result->checkpoint_accelerated = true;
    *used_checkpoint_path = true;
    ++impl_->statistics.layout_calls;
    impl_->publish_cache_metrics(before, result);
    return true;
}

void ZenithHotScrollSession::clear_source_window_cache() noexcept {
    impl_->source_cache.clear();
    impl_->source_lru.clear();
    impl_->statistics.source_window_cache_bytes = 0U;
}

const ZenithHotScrollStats& ZenithHotScrollSession::stats() const noexcept {
    return impl_->statistics;
}

std::uint64_t ZenithHotScrollSession::total_height_q8() const noexcept {
    return impl_->arena.stats().total_height_q8;
}

std::string zenith_hot_scroll_stats_json(const ZenithHotScrollStats& stats) {
    std::ostringstream output;
    output << "{\"layout_calls\":" << stats.layout_calls
           << ",\"checkpoint_cache_hits\":" << stats.checkpoint_cache_hits
           << ",\"checkpoint_cache_misses\":" << stats.checkpoint_cache_misses
           << ",\"checkpoint_cache_evictions\":" << stats.checkpoint_cache_evictions
           << ",\"source_window_cache_hits\":" << stats.source_window_cache_hits
           << ",\"source_window_cache_misses\":" << stats.source_window_cache_misses
           << ",\"source_window_cache_evictions\":" << stats.source_window_cache_evictions
           << ",\"checkpoint_cache_bytes\":" << stats.checkpoint_cache_bytes
           << ",\"checkpoint_cache_peak_bytes\":" << stats.checkpoint_cache_peak_bytes
           << ",\"source_window_cache_bytes\":" << stats.source_window_cache_bytes
           << ",\"source_window_cache_peak_bytes\":" << stats.source_window_cache_peak_bytes
           << '}';
    return output.str();
}

} // namespace zevryon::massivedoc
