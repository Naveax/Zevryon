#include "layout_window.hpp"

#include "compact_document.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <list>
#include <sstream>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

struct CacheKey {
    std::uint64_t record_index{0};
    std::uint32_t width_q8{0};
    std::uint32_t average_advance_q8{0};
    std::uint32_t line_height_q8{0};
    std::uint32_t horizontal_padding_q8{0};
    std::uint32_t vertical_padding_q8{0};

    bool operator==(const CacheKey&) const noexcept = default;
};

struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const noexcept {
        std::size_t value = static_cast<std::size_t>(key.record_index ^ (key.record_index >> 32U));
        const auto mix = [&value](std::uint32_t part) {
            value ^= static_cast<std::size_t>(part) + static_cast<std::size_t>(0x9e3779b9U) + (value << 6U) +
                     (value >> 2U);
        };
        mix(key.width_q8);
        mix(key.average_advance_q8);
        mix(key.line_height_q8);
        mix(key.horizontal_padding_q8);
        mix(key.vertical_padding_q8);
        return value;
    }
};

struct CacheEntry {
    std::uint32_t measured_height_q8{0};
    bool height_saturated{false};
    bool complete_fragments{false};
    std::vector<LayoutFragment> fragments;
    std::size_t charge_bytes{0};
    std::list<CacheKey>::iterator lru_position;
};

struct ScanResult {
    std::uint32_t measured_height_q8{0};
    bool height_saturated{false};
    bool complete_fragments{false};
    bool visible_truncated{false};
    std::uint64_t bytes_read{0};
    std::vector<LayoutFragment> fragments;
    std::vector<LayoutFragment> visible_fragments;
};

constexpr std::size_t kCacheContainerPointerSlots = 12U;
constexpr std::size_t kCacheFixedCharge =
    sizeof(CacheEntry) + sizeof(CacheKey) + kCacheContainerPointerSlots * sizeof(void*);

std::uint32_t bucket_width(std::uint32_t width_q8, std::uint32_t bucket_q8) noexcept {
    if (bucket_q8 == 0U || width_q8 < bucket_q8) {
        return width_q8;
    }
    return static_cast<std::uint32_t>((width_q8 / bucket_q8) * bucket_q8);
}

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

std::size_t conservative_cache_charge(const std::vector<LayoutFragment>& fragments) noexcept {
    if (fragments.capacity() >
        (std::numeric_limits<std::size_t>::max() - kCacheFixedCharge) / sizeof(LayoutFragment)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return kCacheFixedCharge + fragments.capacity() * sizeof(LayoutFragment);
}

void release_fragment_storage(std::vector<LayoutFragment>* fragments) {
    std::vector<LayoutFragment> empty;
    empty.swap(*fragments);
}

bool intersects(
    std::uint64_t start,
    std::uint64_t height,
    std::uint64_t query_start,
    std::uint64_t query_end) noexcept {
    const std::uint64_t end = saturating_add(start, height);
    return end > query_start && start < query_end;
}

} // namespace

struct LayoutWindowEngine::Impl {
    explicit Impl(const std::filesystem::path& root_value, LayoutConfig config_value)
        : root(root_value), config(config_value), store(root_value), arena(root_value) {}

    std::filesystem::path root;
    LayoutConfig config;
    StoreReader store;
    CompactArenaReader arena;
    bool opened{false};
    std::size_t cache_bytes_value{0};
    std::list<CacheKey> lru;
    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> cache;

    CacheKey key_for(std::uint64_t record_index, std::uint32_t viewport_width_q8) const noexcept {
        return CacheKey{
            record_index,
            bucket_width(viewport_width_q8, config.width_bucket_q8),
            config.average_advance_q8,
            config.line_height_q8,
            config.horizontal_padding_q8,
            config.vertical_padding_q8,
        };
    }

    CacheEntry* find_cache(const CacheKey& key) {
        const auto found = cache.find(key);
        if (found == cache.end()) {
            return nullptr;
        }
        lru.splice(lru.begin(), lru, found->second.lru_position);
        found->second.lru_position = lru.begin();
        return &found->second;
    }

    void erase_cache_entry(std::unordered_map<CacheKey, CacheEntry, CacheKeyHash>::iterator entry) {
        if (entry->second.charge_bytes <= cache_bytes_value) {
            cache_bytes_value -= entry->second.charge_bytes;
        } else {
            cache_bytes_value = 0U;
        }
        lru.erase(entry->second.lru_position);
        cache.erase(entry);
    }

    void evict_for(std::size_t incoming) {
        while (!lru.empty() &&
               (incoming > config.max_cache_bytes || cache_bytes_value > config.max_cache_bytes - incoming)) {
            const CacheKey key = lru.back();
            const auto found = cache.find(key);
            if (found == cache.end()) {
                lru.pop_back();
                continue;
            }
            erase_cache_entry(found);
        }
    }

    void store_cache(const CacheKey& key, ScanResult scan) {
        if (config.max_cache_bytes < kCacheFixedCharge) {
            return;
        }

        CacheEntry entry;
        entry.measured_height_q8 = scan.measured_height_q8;
        entry.height_saturated = scan.height_saturated;
        entry.complete_fragments = scan.complete_fragments;
        entry.fragments = std::move(scan.fragments);
        entry.charge_bytes = conservative_cache_charge(entry.fragments);
        if (entry.charge_bytes > config.max_cache_bytes) {
            return;
        }

        const auto existing = cache.find(key);
        if (existing != cache.end()) {
            erase_cache_entry(existing);
        }
        evict_for(entry.charge_bytes);
        if (entry.charge_bytes > config.max_cache_bytes ||
            cache_bytes_value > config.max_cache_bytes - entry.charge_bytes) {
            return;
        }

        lru.push_front(key);
        entry.lru_position = lru.begin();
        const std::size_t charge = entry.charge_bytes;
        const auto inserted = cache.emplace(key, std::move(entry));
        if (!inserted.second) {
            lru.pop_front();
            return;
        }
        cache_bytes_value += charge;
    }

    bool scan_record(
        const MaterializedRecord& record,
        const CacheKey& key,
        std::size_t fragment_cache_budget,
        std::uint64_t visible_start_q8,
        std::uint64_t visible_end_q8,
        std::size_t visible_limit,
        ScanResult* output,
        std::string* error) const {
        if (output == nullptr || error == nullptr || key.width_q8 == 0U || config.average_advance_q8 == 0U ||
            config.line_height_q8 == 0U) {
            if (error != nullptr) {
                *error = "invalid layout scan request";
            }
            return false;
        }
        error->clear();
        *output = ScanResult{};
        const std::uint64_t horizontal = static_cast<std::uint64_t>(config.horizontal_padding_q8) * 2U;
        const std::uint64_t content_width = key.width_q8 > horizontal
                                                ? static_cast<std::uint64_t>(key.width_q8) - horizontal
                                                : static_cast<std::uint64_t>(config.average_advance_q8);
        const std::uint64_t columns_per_line = std::max<std::uint64_t>(
            1U, content_width / std::max<std::uint64_t>(1U, config.average_advance_q8));
        const std::uint64_t top_padding = config.vertical_padding_q8 / 2U;
        std::uint64_t line_start = 0U;
        std::uint64_t source_offset = 0U;
        std::uint64_t columns = 0U;
        std::uint64_t line_count = 0U;
        std::uint64_t pending_start = 0U;
        std::uint8_t pending_remaining = 0U;
        bool emitted_any = false;
        const bool cache_enabled = fragment_cache_budget >= kCacheFixedCharge;
        bool cache_complete = cache_enabled;

        const auto emit_line = [&](std::uint64_t source_end, bool hard_break) {
            const std::uint64_t local_y = saturating_add(
                top_padding, saturating_multiply(line_count, config.line_height_q8));
            const std::uint64_t raw_width = saturating_multiply(columns, config.average_advance_q8);
            LayoutFragment fragment;
            fragment.record_index = record.record_index;
            fragment.logical_id = record.logical_id;
            fragment.source_start = line_start;
            fragment.source_end = source_end;
            fragment.x_q8 = config.horizontal_padding_q8;
            fragment.y_q8 = local_y;
            fragment.width_q8 = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                std::max<std::uint64_t>(config.average_advance_q8, raw_width),
                std::min<std::uint64_t>(content_width, std::numeric_limits<std::uint32_t>::max())));
            fragment.height_q8 = config.line_height_q8;
            fragment.hard_break = hard_break;

            if (cache_complete) {
                output->fragments.push_back(fragment);
                if (conservative_cache_charge(output->fragments) > fragment_cache_budget) {
                    release_fragment_storage(&output->fragments);
                    cache_complete = false;
                }
            }
            if (visible_limit != 0U && intersects(local_y, config.line_height_q8, visible_start_q8, visible_end_q8)) {
                if (output->visible_fragments.size() < visible_limit) {
                    output->visible_fragments.push_back(fragment);
                } else {
                    output->visible_truncated = true;
                }
            }
            ++line_count;
            line_start = source_end;
            columns = 0U;
            emitted_any = true;
        };

        const auto consume_codepoint = [&](std::uint64_t start, std::uint64_t end, std::uint32_t value) {
            if (value == 0x0aU) {
                emit_line(end, true);
                return;
            }
            if (value == 0x09U) {
                columns = saturating_add(columns, 4U);
            } else {
                columns = saturating_add(columns, 1U);
            }
            if (columns >= columns_per_line) {
                emit_line(end, false);
            }
            static_cast<void>(start);
        };

        const bool read_ok = store.read_record(
            record.record_index,
            [&](std::span<const std::byte> bytes) {
                output->bytes_read += static_cast<std::uint64_t>(bytes.size());
                for (const std::byte raw : bytes) {
                    const std::uint8_t byte = static_cast<std::uint8_t>(std::to_integer<unsigned int>(raw));
                    bool retry = true;
                    while (retry) {
                        retry = false;
                        if (pending_remaining != 0U) {
                            if ((byte & 0xc0U) == 0x80U) {
                                --pending_remaining;
                                ++source_offset;
                                if (pending_remaining == 0U) {
                                    consume_codepoint(pending_start, source_offset, 0xfffdU);
                                }
                                continue;
                            }
                            consume_codepoint(pending_start, source_offset, 0xfffdU);
                            pending_remaining = 0U;
                            retry = true;
                            continue;
                        }
                        const std::uint64_t start = source_offset;
                        ++source_offset;
                        if (byte < 0x80U) {
                            consume_codepoint(start, source_offset, byte);
                        } else if ((byte & 0xe0U) == 0xc0U) {
                            pending_start = start;
                            pending_remaining = 1U;
                        } else if ((byte & 0xf0U) == 0xe0U) {
                            pending_start = start;
                            pending_remaining = 2U;
                        } else if ((byte & 0xf8U) == 0xf0U) {
                            pending_start = start;
                            pending_remaining = 3U;
                        } else {
                            consume_codepoint(start, source_offset, 0xfffdU);
                        }
                    }
                }
                return true;
            },
            error);
        if (!read_ok) {
            return false;
        }
        if (pending_remaining != 0U) {
            consume_codepoint(pending_start, source_offset, 0xfffdU);
        }
        if (line_start < source_offset || !emitted_any || columns != 0U) {
            emit_line(source_offset, false);
        }
        const std::uint64_t lines_height = saturating_multiply(line_count, config.line_height_q8);
        const std::uint64_t measured = saturating_add(lines_height, config.vertical_padding_q8);
        output->height_saturated = measured > std::numeric_limits<std::uint32_t>::max();
        output->measured_height_q8 = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(measured, std::numeric_limits<std::uint32_t>::max()));
        output->complete_fragments = cache_complete;
        return true;
    }
};

LayoutWindowEngine::LayoutWindowEngine(const std::filesystem::path& store_root, LayoutConfig config)
    : impl_(std::make_unique<Impl>(store_root, config)) {}

LayoutWindowEngine::~LayoutWindowEngine() = default;

bool LayoutWindowEngine::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->config.average_advance_q8 == 0U || impl_->config.line_height_q8 == 0U ||
        impl_->config.width_bucket_q8 == 0U) {
        *error = "invalid layout configuration";
        return false;
    }
    if (!impl_->store.open(error) || !impl_->arena.open(error)) {
        return false;
    }
    impl_->opened = true;
    return true;
}

bool LayoutWindowEngine::layout(
    std::uint64_t scroll_y_q8,
    std::uint32_t viewport_width_q8,
    std::uint64_t viewport_height_q8,
    std::uint64_t overscan_q8,
    std::size_t max_fragments,
    LayoutWindowResult* result,
    std::string* error) {
    if (!impl_->opened || result == nullptr || error == nullptr || viewport_width_q8 == 0U ||
        viewport_height_q8 == 0U || max_fragments == 0U) {
        if (error != nullptr) {
            *error = "invalid layout window request";
        }
        return false;
    }
    error->clear();
    *result = LayoutWindowResult{};
    ViewportResult initial;
    if (!impl_->arena.materialize(
            scroll_y_q8, viewport_height_q8, overscan_q8, max_fragments, &initial, error)) {
        return false;
    }
    result->query_start_q8 = initial.query_start_q8;
    result->query_end_q8 = initial.query_end_q8;
    result->truncated = initial.truncated;

    for (const MaterializedRecord& record : initial.records) {
        const CacheKey key = impl_->key_for(record.record_index, viewport_width_q8);
        CacheEntry* cached = impl_->find_cache(key);
        std::uint32_t measured_height = 0U;
        bool saturated = false;
        if (cached != nullptr) {
            ++result->cache_hits;
            measured_height = cached->measured_height_q8;
            saturated = cached->height_saturated;
        } else {
            ++result->cache_misses;
            ScanResult scan;
            if (!impl_->scan_record(
                    record,
                    key,
                    impl_->config.max_cache_bytes,
                    0U,
                    0U,
                    0U,
                    &scan,
                    error)) {
                return false;
            }
            result->source_bytes_read += scan.bytes_read;
            measured_height = scan.measured_height_q8;
            saturated = scan.height_saturated;
            impl_->store_cache(key, std::move(scan));
        }
        ++result->measured_records;
        result->height_saturated = result->height_saturated || saturated;
        if (record.height_q8 != measured_height) {
            HeightUpdateResult update;
            if (!impl_->arena.update_height(record.record_index, measured_height, &update, error)) {
                return false;
            }
        }
    }

    ViewportResult corrected;
    if (!impl_->arena.materialize(
            scroll_y_q8, viewport_height_q8, overscan_q8, max_fragments, &corrected, error)) {
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
        const CacheKey key = impl_->key_for(record.record_index, viewport_width_q8);
        CacheEntry* cached = impl_->find_cache(key);
        ScanResult scan;
        const std::vector<LayoutFragment>* local_fragments = nullptr;
        if (cached != nullptr && cached->complete_fragments) {
            local_fragments = &cached->fragments;
        } else {
            const std::uint64_t local_start = corrected.query_start_q8 > record.y_q8
                                                  ? corrected.query_start_q8 - record.y_q8
                                                  : 0U;
            const std::uint64_t local_end = corrected.query_end_q8 > record.y_q8
                                                ? corrected.query_end_q8 - record.y_q8
                                                : 0U;
            const std::size_t remaining = max_fragments - result->fragments.size();
            if (!impl_->scan_record(record, key, 0U, local_start, local_end, remaining, &scan, error)) {
                return false;
            }
            result->source_bytes_read += scan.bytes_read;
            result->truncated = result->truncated || scan.visible_truncated;
            local_fragments = &scan.visible_fragments;
        }

        for (const LayoutFragment& cached_fragment : *local_fragments) {
            const std::uint64_t absolute_y = saturating_add(record.y_q8, cached_fragment.y_q8);
            if (!intersects(
                    absolute_y,
                    cached_fragment.height_q8,
                    corrected.query_start_q8,
                    corrected.query_end_q8)) {
                continue;
            }
            if (result->fragments.size() >= max_fragments) {
                result->truncated = true;
                break;
            }
            LayoutFragment fragment = cached_fragment;
            fragment.y_q8 = absolute_y;
            result->fragments.push_back(fragment);
        }
    }
    result->cache_bytes = impl_->cache_bytes_value;
    return true;
}

std::size_t LayoutWindowEngine::cache_bytes() const noexcept {
    return impl_->cache_bytes_value;
}

std::string layout_window_json(const LayoutWindowResult& result) {
    std::ostringstream output;
    output << "{\"query_start_q8\":" << result.query_start_q8 << ",\"query_end_q8\":"
           << result.query_end_q8 << ",\"total_height_q8\":" << result.total_height_q8
           << ",\"source_bytes_read\":" << result.source_bytes_read << ",\"measured_records\":"
           << result.measured_records << ",\"cache_hits\":" << result.cache_hits << ",\"cache_misses\":"
           << result.cache_misses << ",\"cache_bytes\":" << result.cache_bytes << ",\"truncated\":"
           << (result.truncated ? "true" : "false") << ",\"height_saturated\":"
           << (result.height_saturated ? "true" : "false") << ",\"fragment_count\":"
           << result.fragments.size() << ",\"fragments\":[";
    bool first = true;
    for (const LayoutFragment& fragment : result.fragments) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << "{\"record_index\":" << fragment.record_index << ",\"logical_id\":" << fragment.logical_id
               << ",\"source_start\":" << fragment.source_start << ",\"source_end\":"
               << fragment.source_end << ",\"x_q8\":" << fragment.x_q8 << ",\"y_q8\":" << fragment.y_q8
               << ",\"width_q8\":" << fragment.width_q8 << ",\"height_q8\":" << fragment.height_q8
               << ",\"hard_break\":" << (fragment.hard_break ? "true" : "false") << '}';
    }
    output << "]}";
    return output.str();
}

} // namespace zevryon::massivedoc
