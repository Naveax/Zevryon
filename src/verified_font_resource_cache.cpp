#include "verified_font_resource_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace zevryon::text {
namespace {

bool valid_key(VerifiedFontResourceCacheKey key) noexcept {
    return key.high != 0U || key.low != 0U;
}

bool key_less(
    VerifiedFontResourceCacheKey left,
    VerifiedFontResourceCacheKey right) noexcept {
    if (left.high != right.high) {
        return left.high < right.high;
    }
    if (left.low != right.low) {
        return left.low < right.low;
    }
    return left.face_index < right.face_index;
}

void clear_error(VerifiedFontResourceCacheError* error) noexcept {
    if (error != nullptr) {
        error->kind = VerifiedFontResourceCacheErrorKind::None;
        error->resource_error = VerifiedFontResourceErrorKind::None;
        error->byte_offset = 0U;
        error->table_tag = 0U;
        error->parse_error = SfntParseErrorKind::None;
        error->integrity_error = SfntIntegrityErrorKind::None;
        error->message.clear();
    }
}

bool fail(
    VerifiedFontResourceCacheErrorKind kind,
    const char* message,
    VerifiedFontResourceCacheError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool same_bytes(
    std::span<const std::byte> left,
    std::span<const std::byte> right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin());
}

} // namespace

VerifiedFontResourceCache::VerifiedFontResourceCache(
    std::size_t retention_hard_limit,
    std::size_t metadata_hard_limit,
    std::size_t maximum_entries) noexcept
    : metadata_resource_(
          ledger_, core::ResourceClass::FontResourceCacheMetadata),
      entries_(&metadata_resource_),
      inflight_(&metadata_resource_),
      retention_hard_limit_(retention_hard_limit),
      maximum_entries_(maximum_entries) {
    ledger_.set_hard_limit(
        core::ResourceClass::FontResourceCacheMetadata,
        metadata_hard_limit);
    ledger_.set_hard_limit(
        core::ResourceClass::FontResourceCacheRetention,
        retention_hard_limit);
}

const char* verified_font_resource_cache_error_kind_name(
    VerifiedFontResourceCacheErrorKind kind) noexcept {
    switch (kind) {
    case VerifiedFontResourceCacheErrorKind::None:
        return "none";
    case VerifiedFontResourceCacheErrorKind::InvalidArgument:
        return "invalid_argument";
    case VerifiedFontResourceCacheErrorKind::CacheMiss:
        return "cache_miss";
    case VerifiedFontResourceCacheErrorKind::SourceExceedsRetentionLimit:
        return "source_exceeds_retention_limit";
    case VerifiedFontResourceCacheErrorKind::KeyCollision:
        return "key_collision";
    case VerifiedFontResourceCacheErrorKind::MetadataBudgetExceeded:
        return "metadata_budget_exceeded";
    case VerifiedFontResourceCacheErrorKind::AllocationFailed:
        return "allocation_failed";
    case VerifiedFontResourceCacheErrorKind::ResourceBuildFailed:
        return "resource_build_failed";
    }
    return "unknown";
}

bool VerifiedFontResourceCache::lookup(
    VerifiedFontResourceCacheKey key,
    std::shared_ptr<const VerifiedFontResource>* output,
    VerifiedFontResourceCacheStats* stats,
    VerifiedFontResourceCacheError* error) noexcept {
    return acquire(key, {}, false, output, stats, error);
}

bool VerifiedFontResourceCache::get_or_build(
    VerifiedFontResourceCacheKey key,
    std::span<const std::byte> source,
    std::shared_ptr<const VerifiedFontResource>* output,
    VerifiedFontResourceCacheStats* stats,
    VerifiedFontResourceCacheError* error) noexcept {
    return acquire(key, source, true, output, stats, error);
}

bool VerifiedFontResourceCache::acquire(
    VerifiedFontResourceCacheKey key,
    std::span<const std::byte> source,
    bool allow_build,
    std::shared_ptr<const VerifiedFontResource>* output,
    VerifiedFontResourceCacheStats* stats,
    VerifiedFontResourceCacheError* error) noexcept {
    if (output != nullptr) {
        output->reset();
    }
    if (stats != nullptr) {
        *stats = {};
    }
    clear_error(error);

    if (output == nullptr || !valid_key(key) || retention_hard_limit_ == 0U ||
        maximum_entries_ == 0U || (allow_build && source.empty())) {
        if (stats != nullptr) {
            *stats = snapshot();
        }
        return fail(
            VerifiedFontResourceCacheErrorKind::InvalidArgument,
            "cache, key, output, and build source must be valid",
            error);
    }
    if (allow_build && source.size() > retention_hard_limit_) {
        if (stats != nullptr) {
            *stats = snapshot();
        }
        return fail(
            VerifiedFontResourceCacheErrorKind::SourceExceedsRetentionLimit,
            "font source exceeds the cache retention hard limit",
            error);
    }

    bool miss_recorded = false;
    try {
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex_);

            const auto entry = std::find_if(
                entries_.begin(),
                entries_.end(),
                [key](const Entry& candidate) {
                    return candidate.key == key;
                });
            if (entry != entries_.end()) {
                if (allow_build &&
                    !same_bytes(source, entry->resource->bytes())) {
                    ++key_collisions_;
                    if (stats != nullptr) {
                        *stats = snapshot_locked();
                    }
                    lock.unlock();
                    return fail(
                        VerifiedFontResourceCacheErrorKind::KeyCollision,
                        "cache key already identifies different font bytes",
                        error);
                }
                ++hits_;
                ++use_epoch_;
                if (use_epoch_ == 0U) {
                    use_epoch_ = 1U;
                }
                entry->last_use_epoch = use_epoch_;
                *output = entry->resource;
                if (stats != nullptr) {
                    *stats = snapshot_locked();
                }
                return true;
            }

            if (!miss_recorded) {
                ++misses_;
                miss_recorded = true;
            }
            if (!allow_build) {
                if (stats != nullptr) {
                    *stats = snapshot_locked();
                }
                lock.unlock();
                return fail(
                    VerifiedFontResourceCacheErrorKind::CacheMiss,
                    "verified font resource is not resident",
                    error);
            }

            const auto in_flight = std::find_if(
                inflight_.begin(),
                inflight_.end(),
                [key](const InFlight& candidate) {
                    return candidate.key == key;
                });
            if (in_flight != inflight_.end()) {
                ++waits_;
                condition_.wait(lock, [this, key] {
                    return std::none_of(
                        inflight_.begin(),
                        inflight_.end(),
                        [key](const InFlight& candidate) {
                            return candidate.key == key;
                        });
                });
                continue;
            }

            const std::uint64_t resource_id = next_resource_id_;
            ++next_resource_id_;
            if (next_resource_id_ == 0U) {
                next_resource_id_ = 1U;
            }
            try {
                inflight_.push_back(InFlight{key, resource_id});
            } catch (const std::bad_alloc&) {
                const bool budget_rejected = ledger_.snapshot(
                    core::ResourceClass::FontResourceCacheMetadata)
                        .rejected_reservations != 0U;
                if (stats != nullptr) {
                    *stats = snapshot_locked();
                }
                lock.unlock();
                return fail(
                    budget_rejected
                        ? VerifiedFontResourceCacheErrorKind::MetadataBudgetExceeded
                        : VerifiedFontResourceCacheErrorKind::AllocationFailed,
                    budget_rejected
                        ? "cache in-flight metadata exceeded its hard limit"
                        : "cache in-flight metadata allocation failed",
                    error);
            }
            ++build_attempts_;
            lock.unlock();

            std::shared_ptr<const VerifiedFontResource> candidate;
            VerifiedFontResourceStats resource_stats;
            VerifiedFontResourceError resource_error;
            const bool built = build_verified_font_resource(
                resource_id,
                source,
                key.face_index,
                source.size(),
                &candidate,
                &resource_stats,
                &resource_error);

            lock.lock();
            const auto completed = std::find_if(
                inflight_.begin(),
                inflight_.end(),
                [key](const InFlight& value) {
                    return value.key == key;
                });
            if (completed != inflight_.end()) {
                inflight_.erase(completed);
            }

            if (!built) {
                ++build_failures_;
                if (stats != nullptr) {
                    *stats = snapshot_locked();
                }
                lock.unlock();
                condition_.notify_all();
                if (error != nullptr) {
                    error->kind =
                        VerifiedFontResourceCacheErrorKind::ResourceBuildFailed;
                    error->resource_error = resource_error.kind;
                    error->byte_offset = resource_error.byte_offset;
                    error->table_tag = resource_error.table_tag;
                    error->parse_error = resource_error.parse_error;
                    error->integrity_error = resource_error.integrity_error;
                    try {
                        error->message = "verified resource build failed: ";
                        error->message += verified_font_resource_error_kind_name(
                            resource_error.kind);
                    } catch (...) {
                        error->message.clear();
                    }
                }
                return false;
            }

            const std::size_t retained_bytes = candidate->bytes().size();
            while (entries_.size() >= maximum_entries_ ||
                   ledger_.snapshot(
                       core::ResourceClass::FontResourceCacheRetention)
                           .current_bytes >
                       retention_hard_limit_ - retained_bytes) {
                if (entries_.empty()) {
                    ++build_failures_;
                    if (stats != nullptr) {
                        *stats = snapshot_locked();
                    }
                    lock.unlock();
                    condition_.notify_all();
                    return fail(
                        VerifiedFontResourceCacheErrorKind::SourceExceedsRetentionLimit,
                        "verified resource cannot fit the cache retention limit",
                        error);
                }
                auto victim = entries_.begin();
                for (auto current = entries_.begin() + 1;
                     current != entries_.end();
                     ++current) {
                    if (current->last_use_epoch < victim->last_use_epoch ||
                        (current->last_use_epoch == victim->last_use_epoch &&
                         key_less(current->key, victim->key))) {
                        victim = current;
                    }
                }
                ledger_.release(
                    core::ResourceClass::FontResourceCacheRetention,
                    victim->retained_bytes);
                entries_.erase(victim);
                ++evictions_;
            }

            if (!ledger_.try_reserve(
                    core::ResourceClass::FontResourceCacheRetention,
                    retained_bytes)) {
                ++build_failures_;
                if (stats != nullptr) {
                    *stats = snapshot_locked();
                }
                lock.unlock();
                condition_.notify_all();
                return fail(
                    VerifiedFontResourceCacheErrorKind::SourceExceedsRetentionLimit,
                    "verified resource retention reservation was rejected",
                    error);
            }

            ++use_epoch_;
            if (use_epoch_ == 0U) {
                use_epoch_ = 1U;
            }
            try {
                entries_.push_back(Entry{
                    key,
                    candidate,
                    use_epoch_,
                    retained_bytes});
            } catch (const std::bad_alloc&) {
                ledger_.release(
                    core::ResourceClass::FontResourceCacheRetention,
                    retained_bytes);
                ++build_failures_;
                const bool budget_rejected = ledger_.snapshot(
                    core::ResourceClass::FontResourceCacheMetadata)
                        .rejected_reservations != 0U;
                if (stats != nullptr) {
                    *stats = snapshot_locked();
                }
                lock.unlock();
                condition_.notify_all();
                return fail(
                    budget_rejected
                        ? VerifiedFontResourceCacheErrorKind::MetadataBudgetExceeded
                        : VerifiedFontResourceCacheErrorKind::AllocationFailed,
                    budget_rejected
                        ? "cache entry metadata exceeded its hard limit"
                        : "cache entry metadata allocation failed",
                    error);
            }

            ++builds_published_;
            *output = std::move(candidate);
            if (stats != nullptr) {
                *stats = snapshot_locked();
            }
            lock.unlock();
            condition_.notify_all();
            return true;
        }
    } catch (...) {
        if (stats != nullptr) {
            *stats = snapshot();
        }
        return fail(
            VerifiedFontResourceCacheErrorKind::AllocationFailed,
            "unexpected cache synchronization or allocation failure",
            error);
    }
}

void VerifiedFontResourceCache::clear() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Entry& entry : entries_) {
            ledger_.release(
                core::ResourceClass::FontResourceCacheRetention,
                entry.retained_bytes);
        }
        entries_.clear();
        ++clears_;
    } catch (...) {
    }
}

VerifiedFontResourceCacheStats VerifiedFontResourceCache::snapshot() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_locked();
    } catch (...) {
        return {};
    }
}

VerifiedFontResourceCacheStats
VerifiedFontResourceCache::snapshot_locked() const noexcept {
    VerifiedFontResourceCacheStats result;
    result.metadata = ledger_.snapshot(
        core::ResourceClass::FontResourceCacheMetadata);
    result.retention = ledger_.snapshot(
        core::ResourceClass::FontResourceCacheRetention);
    result.hits = hits_;
    result.misses = misses_;
    result.waits = waits_;
    result.build_attempts = build_attempts_;
    result.builds_published = builds_published_;
    result.build_failures = build_failures_;
    result.key_collisions = key_collisions_;
    result.evictions = evictions_;
    result.clears = clears_;
    result.next_resource_id = next_resource_id_;
    result.entry_count = entries_.size();
    result.inflight_count = inflight_.size();
    result.maximum_entries = maximum_entries_;
    return result;
}

} // namespace zevryon::text
