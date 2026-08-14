#pragma once

#include "resource_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace zevryon::massivedoc {

constexpr std::size_t kDefaultImmutableBlockBytes = 64U * 1024U;
constexpr std::size_t kDefaultHotBlockCacheBytes = 256U * 1024U;
constexpr std::size_t kDefaultWarmBlockCacheBytes = 512U * 1024U;
constexpr std::size_t kMaximumImmutableBlockBytes = 1024U * 1024U;
constexpr std::size_t kMaximumResidentBlockCacheBytes = 16U * 1024U * 1024U;

enum class ImmutableBlockTier : std::uint8_t {
    cold = 0U,
    warm = 1U,
    hot = 2U,
};

struct ImmutableBlockCacheConfig {
    std::size_t block_bytes{kDefaultImmutableBlockBytes};
    std::size_t hot_bytes{kDefaultHotBlockCacheBytes};
    std::size_t warm_bytes{kDefaultWarmBlockCacheBytes};
};

struct ImmutableBlockCacheStats {
    std::size_t hot_resident_bytes{0U};
    std::size_t warm_resident_bytes{0U};
    std::size_t resident_bytes{0U};
    std::size_t peak_resident_bytes{0U};
    std::size_t hot_blocks{0U};
    std::size_t warm_blocks{0U};
    std::uint64_t hot_hits{0U};
    std::uint64_t warm_hits{0U};
    std::uint64_t cold_misses{0U};
    std::uint64_t promotions{0U};
    std::uint64_t demotions{0U};
    std::uint64_t evictions{0U};
    std::uint64_t physical_read_bytes{0U};
    bool ledger_within_hard_limits{true};
    bool ledger_accounting_clean{true};
};

using ImmutableBlockLoader = std::function<bool(
    std::uint32_t source_id,
    std::uint64_t block_offset,
    std::size_t maximum_bytes,
    std::vector<std::byte>* output,
    std::string* error)>;

bool validate_immutable_block_cache_config(
    ImmutableBlockCacheConfig config,
    std::string* error);

class ImmutableBlockCache final {
public:
    explicit ImmutableBlockCache(ImmutableBlockCacheConfig config = {});
    ~ImmutableBlockCache();

    ImmutableBlockCache(const ImmutableBlockCache&) = delete;
    ImmutableBlockCache& operator=(const ImmutableBlockCache&) = delete;
    ImmutableBlockCache(ImmutableBlockCache&&) = delete;
    ImmutableBlockCache& operator=(ImmutableBlockCache&&) = delete;

    bool read_exact(
        std::uint32_t source_id,
        std::uint64_t offset,
        std::span<std::byte> destination,
        const ImmutableBlockLoader& loader,
        std::string* error);

    void evict_all_to_cold() noexcept;
    ImmutableBlockCacheStats stats() const noexcept;
    ImmutableBlockCacheConfig config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc
