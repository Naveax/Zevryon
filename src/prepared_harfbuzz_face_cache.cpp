#include "prepared_harfbuzz_face_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace zevryon::text {
namespace {

bool valid_content_identity(FontContentIdentity identity) noexcept {
    return identity.high != 0U || identity.low != 0U;
}

bool valid_key(PreparedHarfBuzzFaceCacheKey key) noexcept {
    return key.generation_id != 0U &&
        key.face_id != kInvalidFontFaceId &&
        valid_content_identity(key.content_identity);
}

bool key_less(
    PreparedHarfBuzzFaceCacheKey left,
    PreparedHarfBuzzFaceCacheKey right) noexcept {
    if (left.generation_id != right.generation_id) {
        return left.generation_id < right.generation_id;
    }
    if (left.generation_fingerprint.high !=
        right.generation_fingerprint.high) {
        return left.generation_fingerprint.high <
            right.generation_fingerprint.high;
    }
    if (left.generation_fingerprint.low !=
        right.generation_fingerprint.low) {
        return left.generation_fingerprint.low <
            right.generation_fingerprint.low;
    }
    if (left.face_id != right.face_id) {
        return left.face_id < right.face_id;
    }
    if (left.content_identity.high != right.content_identity.high) {
        return left.content_identity.high < right.content_identity.high;
    }
    if (left.content_identity.low != right.content_identity.low) {
        return left.content_identity.low < right.content_identity.low;
    }
    return left.content_identity.face_index <
        right.content_identity.face_index;
}

bool same_bytes(
    std::span<const std::byte> left,
    std::span<const std::byte> right) noexcept {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin());
}

void clear_error(PreparedHarfBuzzFaceCacheError* error) noexcept {
    if (error != nullptr) {
        error->kind = PreparedHarfBuzzFaceCacheErrorKind::None;
        error->face_error = PreparedHarfBuzzFaceErrorKind::None;
        error->message.clear();
    }
}

bool fail(
    PreparedHarfBuzzFaceCacheErrorKind kind,
    const char* message,
    PreparedHarfBuzzFaceCacheError* error) noexcept {
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

bool entry_matches_binding(
    const PreparedHarfBuzzFace& face,
    const CatalogFontFaceBinding& binding) noexcept {
    if (!face.valid() || !binding.valid() ||
        prepared_harfbuzz_face_cache_key(face.binding()) !=
            prepared_harfbuzz_face_cache_key(binding)) {
        return false;
    }
    const std::shared_ptr<const VerifiedFontResource>& cached_resource =
        face.binding().resource();
    const std::shared_ptr<const VerifiedFontResource>& source_resource =
        binding.resource();
    if (!cached_resource || !source_resource) {
        return false;
    }
    return cached_resource.get() == source_resource.get() ||
        same_bytes(cached_resource->bytes(), source_resource->bytes());
}

} // namespace

PreparedHarfBuzzFaceCacheKey prepared_harfbuzz_face_cache_key(
    const CatalogFontFaceBinding& binding) noexcept {
    if (!binding.valid()) {
        return {};
    }
    PreparedHarfBuzzFaceCacheKey key;
    key.generation_id = binding.generation_id();
    key.generation_fingerprint = binding.generation_fingerprint();
    key.content_identity = binding.content_identity();
    key.face_id = binding.face_id();
    return key;
}

PreparedHarfBuzzFaceCache::PreparedHarfBuzzFaceCache(
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

const char* prepared_harfbuzz_face_cache_error_kind_name(
    PreparedHarfBuzzFaceCacheErrorKind kind) noexcept {
    switch (kind) {
    case PreparedHarfBuzzFaceCacheErrorKind::None:
        return "none";
    case PreparedHarfBuzzFaceCacheErrorKind::InvalidArgument:
        return "invalid_argument";
    case PreparedHarfBuzzFaceCacheErrorKind::CacheMiss:
        return "cache_miss";
    case PreparedHarfBuzzFaceCacheErrorKind::BindingExceedsRetentionLimit:
        return "binding_exceeds_retention_limit";
    case PreparedHarfBuzzFaceCacheErrorKind::KeyCollision:
        return "key_collision";
    case PreparedHarfBuzzFaceCacheErrorKind::MetadataBudgetExceeded:
        return "metadata_budget_exceeded";
    case PreparedHarfBuzzFaceCacheErrorKind::AllocationFailed:
        return "allocation_failed";
    case PreparedHarfBuzzFaceCacheErrorKind::FacePreparationFailed:
        return "face_preparation_failed";
    }
    return "unknown";
}

bool PreparedHarfBuzzFaceCache::lookup(
    CatalogFontFaceBinding binding,
    std::shared_ptr<const PreparedHarfBuzzFace>* output,
    PreparedHarfBuzzFaceCacheStats* stats,
    PreparedHarfBuzzFaceCacheError* error) noexcept {
    return acquire(std::move(binding), false, output, stats, error);
}

bool PreparedHarfBuzzFaceCache::get_or_prepare(
    CatalogFontFaceBinding binding,
    std::shared_ptr<const PreparedHarfBuzzFace>* output,
    PreparedHarfBuzzFaceCacheStats* stats,
    PreparedHarfBuzzFaceCacheError* error) noexcept {
    return acquire(std::move(binding), true, output, stats, error);
}

bool PreparedHarfBuzzFaceCache::acquire(
    CatalogFontFaceBinding binding,
    bool allow_prepare,
    std::shared_ptr<const PreparedHarfBuzzFace>* output,
    PreparedHarfBuzzFaceCacheStats* stats,
    PreparedHarfBuzzFaceCacheError* error) noexcept {
    if (output != nullptr) {
        output->reset();
    }
    if (stats != nullptr) {
        *stats = {};
    }
    clear_error(error);

    const PreparedHarfBuzzFaceCacheKey key =
        prepared_harfbuzz_face_cache_key(binding);
    if (output == nullptr || !binding.valid() || !valid_key(key) ||
        retention_hard_limit_ == 0U || maximum_entries_ == 0U) {
        if (stats != nullptr) {
            *stats = snapshot();
        }
        return fail(
            PreparedHarfBuzzFaceCacheErrorKind::InvalidArgument,
            "cache, binding, limits, and output must be valid",
            error);
    }

    const std::size_t retained_bytes = binding.resource()->bytes().size();
    if (retained_bytes == 0U || retained_bytes > retention_hard_limit_) {
        if (stats != nullptr) {
            *stats = snapshot();
        }
        return fail(
            PreparedHarfBuzzFaceCacheErrorKind::BindingExceedsRetentionLimit,
            "catalog binding exceeds the prepared-face retention hard limit",
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
                if (!entry_matches_binding(*entry->face, binding)) {
                    ++key_collisions_;
                    if (stats != nullptr) {
                        *stats = snapshot_locked();
                    }
                    lock.unlock();
                    return fail(
                        PreparedHarfBuzzFaceCacheErrorKind::KeyCollision,
                        "prepared-face cache key identifies different font bytes or catalog semantics",
                        error);
                }
                ++hits_;
                ++use_epoch_;
                if (use_epoch_ == 0U) {
                    use_epoch_ = 1U;
                }
                entry->last_use_epoch = use_epoch_;
                *output = entry->face;
                if (stats != nullptr) {
                    *stats = snapshot_locked();
                }
                return true;
            }

            if (!miss_recorded) {
                ++misses_;
                miss_recorded = true;
            }
            if (!allow_prepare) {
                if (stats != nullptr) {
                    *stats = snapshot_locked();
                }
                lock.unlock();
                return fail(
                    PreparedHarfBuzzFaceCacheErrorKind::CacheMiss,
                    "prepared HarfBuzz face is not resident",
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

            try {
                inflight_.push_back(InFlight{key});
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
                        ? PreparedHarfBuzzFaceCacheErrorKind::MetadataBudgetExceeded
                        : PreparedHarfBuzzFaceCacheErrorKind::AllocationFailed,
                    budget_rejected
                        ? "prepared-face in-flight metadata exceeded its hard limit"
                        : "prepared-face in-flight metadata allocation failed",
                    error);
            }
            ++preparation_attempts_;
            lock.unlock();

            std::shared_ptr<const PreparedHarfBuzzFace> candidate;
            PreparedHarfBuzzFaceStats face_stats;
            PreparedHarfBuzzFaceError face_error;
            const bool prepared = prepare_harfbuzz_face(
                binding,
                &candidate,
                &face_stats,
                &face_error);

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

            if (!prepared || !candidate || !candidate->valid()) {
                ++preparation_failures_;
                if (stats != nullptr) {
                    *stats = snapshot_locked();
                }
                lock.unlock();
                condition_.notify_all();
                if (error != nullptr) {
                    error->kind =
                        PreparedHarfBuzzFaceCacheErrorKind::FacePreparationFailed;
                    error->face_error = face_error.kind;
                    try {
                        error->message = "prepared HarfBuzz face construction failed: ";
                        error->message += prepared_harfbuzz_face_error_kind_name(
                            face_error.kind);
                    } catch (...) {
                        error->message.clear();
                    }
                }
                return false;
            }

            while (entries_.size() >= maximum_entries_ ||
                   ledger_.snapshot(
                       core::ResourceClass::FontResourceCacheRetention)
                           .current_bytes >
                       retention_hard_limit_ - retained_bytes) {
                if (entries_.empty()) {
                    ++preparation_failures_;
                    if (stats != nullptr) {
                        *stats = snapshot_locked();
                    }
                    lock.unlock();
                    condition_.notify_all();
                    return fail(
                        PreparedHarfBuzzFaceCacheErrorKind::BindingExceedsRetentionLimit,
                        "prepared face cannot fit the cache retention limit",
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
                ++preparation_failures_;
                if (stats != nullptr) {
                    *stats = snapshot_locked();
                }
                lock.unlock();
                condition_.notify_all();
                return fail(
                    PreparedHarfBuzzFaceCacheErrorKind::BindingExceedsRetentionLimit,
                    "prepared face retention reservation was rejected",
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
                ++preparation_failures_;
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
                        ? PreparedHarfBuzzFaceCacheErrorKind::MetadataBudgetExceeded
                        : PreparedHarfBuzzFaceCacheErrorKind::AllocationFailed,
                    budget_rejected
                        ? "prepared-face entry metadata exceeded its hard limit"
                        : "prepared-face entry metadata allocation failed",
                    error);
            }

            ++faces_published_;
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
            PreparedHarfBuzzFaceCacheErrorKind::AllocationFailed,
            "unexpected prepared-face cache synchronization or allocation failure",
            error);
    }
}

void PreparedHarfBuzzFaceCache::clear() noexcept {
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

PreparedHarfBuzzFaceCacheStats
PreparedHarfBuzzFaceCache::snapshot() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_locked();
    } catch (...) {
        return {};
    }
}

PreparedHarfBuzzFaceCacheStats
PreparedHarfBuzzFaceCache::snapshot_locked() const noexcept {
    PreparedHarfBuzzFaceCacheStats result;
    result.metadata = ledger_.snapshot(
        core::ResourceClass::FontResourceCacheMetadata);
    result.retention = ledger_.snapshot(
        core::ResourceClass::FontResourceCacheRetention);
    result.hits = hits_;
    result.misses = misses_;
    result.waits = waits_;
    result.preparation_attempts = preparation_attempts_;
    result.faces_published = faces_published_;
    result.preparation_failures = preparation_failures_;
    result.key_collisions = key_collisions_;
    result.evictions = evictions_;
    result.clears = clears_;
    result.entry_count = entries_.size();
    result.inflight_count = inflight_.size();
    result.maximum_entries = maximum_entries_;
    return result;
}

} // namespace zevryon::text
