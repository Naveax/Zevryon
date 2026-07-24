#pragma once

#include "font_content_identity.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "verified_font_resource.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

struct VerifiedFontResourceCacheKey {
    std::uint64_t high{0};
    std::uint64_t low{0};
    std::uint32_t face_index{0};

    bool operator==(const VerifiedFontResourceCacheKey&) const noexcept = default;
};

static_assert(
    sizeof(VerifiedFontResourceCacheKey) <= 24U,
    "verified font cache keys must remain compact");

VerifiedFontResourceCacheKey verified_font_cache_key(
    FontContentIdentity identity) noexcept;

enum class VerifiedFontResourceCacheErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    CacheMiss,
    SourceExceedsRetentionLimit,
    KeyCollision,
    MetadataBudgetExceeded,
    AllocationFailed,
    ResourceBuildFailed
};

struct VerifiedFontResourceCacheError {
    VerifiedFontResourceCacheErrorKind kind{
        VerifiedFontResourceCacheErrorKind::None};
    VerifiedFontResourceErrorKind resource_error{
        VerifiedFontResourceErrorKind::None};
    std::size_t byte_offset{0};
    std::uint32_t table_tag{0};
    SfntParseErrorKind parse_error{SfntParseErrorKind::None};
    SfntIntegrityErrorKind integrity_error{SfntIntegrityErrorKind::None};
    std::string message;
};

struct VerifiedFontResourceCacheStats {
    core::ResourceSnapshot metadata;
    core::ResourceSnapshot retention;
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t waits{0};
    std::uint64_t build_attempts{0};
    std::uint64_t builds_published{0};
    std::uint64_t build_failures{0};
    std::uint64_t key_collisions{0};
    std::uint64_t evictions{0};
    std::uint64_t clears{0};
    std::uint64_t next_resource_id{1};
    std::size_t entry_count{0};
    std::size_t inflight_count{0};
    std::size_t maximum_entries{0};
};

class VerifiedFontResourceCache final {
public:
    VerifiedFontResourceCache(
        std::size_t retention_hard_limit,
        std::size_t metadata_hard_limit,
        std::size_t maximum_entries) noexcept;

    VerifiedFontResourceCache(const VerifiedFontResourceCache&) = delete;
    VerifiedFontResourceCache& operator=(
        const VerifiedFontResourceCache&) = delete;

    // Returns a resident handle without reading source bytes. Cache keys are
    // caller-defined stable content identities; a miss publishes no output.
    bool lookup(
        VerifiedFontResourceCacheKey key,
        std::shared_ptr<const VerifiedFontResource>* output,
        VerifiedFontResourceCacheStats* stats,
        VerifiedFontResourceCacheError* error) noexcept;

    // Returns a resident handle or builds one immutable verified resource on a
    // miss. Concurrent misses for the same key are single-flight. Supplying
    // bytes for an existing key performs a collision guard before returning it.
    bool get_or_build(
        VerifiedFontResourceCacheKey key,
        std::span<const std::byte> source,
        std::shared_ptr<const VerifiedFontResource>* output,
        VerifiedFontResourceCacheStats* stats,
        VerifiedFontResourceCacheError* error) noexcept;

    // Computes the domain-separated SHA-256-derived 128-bit identity from the
    // exact source and face index, then uses the normal collision-guarded cache
    // path. The optional identity output is published before cache lookup/build.
    bool get_or_build_content_addressed(
        std::span<const std::byte> source,
        std::uint32_t face_index,
        std::shared_ptr<const VerifiedFontResource>* output,
        FontContentIdentity* identity,
        VerifiedFontResourceCacheStats* stats,
        VerifiedFontResourceCacheError* error) noexcept;

    void clear() noexcept;
    VerifiedFontResourceCacheStats snapshot() const noexcept;

private:
    struct Entry {
        VerifiedFontResourceCacheKey key;
        std::shared_ptr<const VerifiedFontResource> resource;
        std::uint64_t last_use_epoch{0};
        std::size_t retained_bytes{0};
    };

    struct InFlight {
        VerifiedFontResourceCacheKey key;
        std::uint64_t resource_id{0};
    };

    bool acquire(
        VerifiedFontResourceCacheKey key,
        std::span<const std::byte> source,
        bool allow_build,
        std::shared_ptr<const VerifiedFontResource>* output,
        VerifiedFontResourceCacheStats* stats,
        VerifiedFontResourceCacheError* error) noexcept;

    VerifiedFontResourceCacheStats snapshot_locked() const noexcept;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    core::ResourceLedger ledger_;
    core::LedgerMemoryResource metadata_resource_;
    std::pmr::vector<Entry> entries_;
    std::pmr::vector<InFlight> inflight_;
    std::size_t retention_hard_limit_{0};
    std::size_t maximum_entries_{0};
    std::uint64_t use_epoch_{0};
    std::uint64_t next_resource_id_{1};
    std::uint64_t hits_{0};
    std::uint64_t misses_{0};
    std::uint64_t waits_{0};
    std::uint64_t build_attempts_{0};
    std::uint64_t builds_published_{0};
    std::uint64_t build_failures_{0};
    std::uint64_t key_collisions_{0};
    std::uint64_t evictions_{0};
    std::uint64_t clears_{0};
};

const char* verified_font_resource_cache_error_kind_name(
    VerifiedFontResourceCacheErrorKind kind) noexcept;

} // namespace zevryon::text
