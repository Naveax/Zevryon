#pragma once

#include "ledger_memory_resource.hpp"
#include "prepared_harfbuzz_face.hpp"
#include "resource_ledger.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <string>
#include <vector>

namespace zevryon::text {

struct PreparedHarfBuzzFaceCacheKey {
    std::uint64_t generation_id{0};
    FontGenerationFingerprint generation_fingerprint{};
    FontContentIdentity content_identity{};
    FontFaceId face_id{kInvalidFontFaceId};

    bool operator==(const PreparedHarfBuzzFaceCacheKey&) const noexcept = default;
};

static_assert(
    sizeof(PreparedHarfBuzzFaceCacheKey) <= 64U,
    "prepared HarfBuzz face cache keys must remain compact");

PreparedHarfBuzzFaceCacheKey prepared_harfbuzz_face_cache_key(
    const CatalogFontFaceBinding& binding) noexcept;

enum class PreparedHarfBuzzFaceCacheErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    CacheMiss,
    BindingExceedsRetentionLimit,
    KeyCollision,
    MetadataBudgetExceeded,
    AllocationFailed,
    FacePreparationFailed
};

struct PreparedHarfBuzzFaceCacheError {
    PreparedHarfBuzzFaceCacheErrorKind kind{
        PreparedHarfBuzzFaceCacheErrorKind::None};
    PreparedHarfBuzzFaceErrorKind face_error{
        PreparedHarfBuzzFaceErrorKind::None};
    std::string message;
};

struct PreparedHarfBuzzFaceCacheStats {
    core::ResourceSnapshot metadata;
    core::ResourceSnapshot retention;
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t waits{0};
    std::uint64_t preparation_attempts{0};
    std::uint64_t faces_published{0};
    std::uint64_t preparation_failures{0};
    std::uint64_t key_collisions{0};
    std::uint64_t evictions{0};
    std::uint64_t clears{0};
    std::size_t entry_count{0};
    std::size_t inflight_count{0};
    std::size_t maximum_entries{0};
};

class PreparedHarfBuzzFaceCache final {
public:
    PreparedHarfBuzzFaceCache(
        std::size_t retention_hard_limit,
        std::size_t metadata_hard_limit,
        std::size_t maximum_entries) noexcept;

    PreparedHarfBuzzFaceCache(const PreparedHarfBuzzFaceCache&) = delete;
    PreparedHarfBuzzFaceCache& operator=(
        const PreparedHarfBuzzFaceCache&) = delete;

    // Returns an already prepared immutable face. The supplied binding is used
    // for full semantic-key and exact-byte collision validation. A miss
    // publishes no output.
    bool lookup(
        CatalogFontFaceBinding binding,
        std::shared_ptr<const PreparedHarfBuzzFace>* output,
        PreparedHarfBuzzFaceCacheStats* stats,
        PreparedHarfBuzzFaceCacheError* error) noexcept;

    // Returns a resident face or prepares one immutable HarfBuzz blob/face pair
    // on a miss. Concurrent misses for the same catalog/content key are
    // single-flight. Source-byte retention is conservatively charged per entry;
    // native HarfBuzz heap size is not guessed.
    bool get_or_prepare(
        CatalogFontFaceBinding binding,
        std::shared_ptr<const PreparedHarfBuzzFace>* output,
        PreparedHarfBuzzFaceCacheStats* stats,
        PreparedHarfBuzzFaceCacheError* error) noexcept;

    void clear() noexcept;
    PreparedHarfBuzzFaceCacheStats snapshot() const noexcept;

private:
    struct Entry {
        PreparedHarfBuzzFaceCacheKey key;
        std::shared_ptr<const PreparedHarfBuzzFace> face;
        std::uint64_t last_use_epoch{0};
        std::size_t retained_bytes{0};
    };

    struct InFlight {
        PreparedHarfBuzzFaceCacheKey key;
    };

    bool acquire(
        CatalogFontFaceBinding binding,
        bool allow_prepare,
        std::shared_ptr<const PreparedHarfBuzzFace>* output,
        PreparedHarfBuzzFaceCacheStats* stats,
        PreparedHarfBuzzFaceCacheError* error) noexcept;

    PreparedHarfBuzzFaceCacheStats snapshot_locked() const noexcept;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    core::ResourceLedger ledger_;
    core::LedgerMemoryResource metadata_resource_;
    std::pmr::vector<Entry> entries_;
    std::pmr::vector<InFlight> inflight_;
    std::size_t retention_hard_limit_{0};
    std::size_t maximum_entries_{0};
    std::uint64_t use_epoch_{0};
    std::uint64_t hits_{0};
    std::uint64_t misses_{0};
    std::uint64_t waits_{0};
    std::uint64_t preparation_attempts_{0};
    std::uint64_t faces_published_{0};
    std::uint64_t preparation_failures_{0};
    std::uint64_t key_collisions_{0};
    std::uint64_t evictions_{0};
    std::uint64_t clears_{0};
};

const char* prepared_harfbuzz_face_cache_error_kind_name(
    PreparedHarfBuzzFaceCacheErrorKind kind) noexcept;

} // namespace zevryon::text
