#pragma once

#include "layout_window.hpp"
#include "massivedoc_block_cache.hpp"
#include "massivedoc_cold_window.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zevryon::massivedoc {

constexpr std::size_t kProfileDecimalMb = 1'000'000U;

enum class MassiveDocDeviceProfile : std::uint8_t {
    LegacyPhone = 0,
    MidPhone,
    ModernPhone,
    Desktop,
};

struct MassiveDocProfileLimits {
    MassiveDocDeviceProfile profile{MassiveDocDeviceProfile::LegacyPhone};
    const char* name{"legacy-phone"};
    std::uint32_t minimum_physical_ram_mib{0U};
    std::uint32_t process_group_pss_target_mb{0U};
    std::uint32_t process_group_pss_hard_cap_mb{0U};
    std::uint32_t hot_cache_mb{0U};
    std::uint32_t warm_cache_mb{0U};
    std::uint32_t cold_cache_mb{0U};
    std::uint32_t first_viewport_preindexed_ms{0U};
    std::uint32_t first_viewport_streaming_ms{0U};
    double scroll_p99_ms{0.0};
    std::uint32_t maximum_normal_stall_ms{0U};
    std::uint32_t exact_search_warm_ms{0U};
    std::uint32_t exact_search_cold_ms{0U};
    std::uint32_t mutation_p95_us{0U};
    std::uint32_t copy_throughput_mib_s{0U};
};

inline constexpr std::array<MassiveDocProfileLimits, 4U> kMassiveDocProfileLimits{{
    {MassiveDocDeviceProfile::LegacyPhone, "legacy-phone", 2048U, 64U, 80U, 6U, 6U, 4U,
     5000U, 2000U, 33.3, 250U, 350U, 1500U, 2000U, 60U},
    {MassiveDocDeviceProfile::MidPhone, "mid-phone", 4096U, 80U, 96U, 8U, 8U, 6U,
     2500U, 1000U, 16.6, 100U, 180U, 800U, 1000U, 120U},
    {MassiveDocDeviceProfile::ModernPhone, "modern-phone", 8192U, 96U, 128U, 12U, 12U, 8U,
     1500U, 600U, 11.1, 50U, 120U, 500U, 500U, 200U},
    {MassiveDocDeviceProfile::Desktop, "desktop", 8192U, 128U, 160U, 18U, 18U, 10U,
     1000U, 300U, 8.33, 16U, 100U, 300U, 250U, 400U},
}};

constexpr const MassiveDocProfileLimits& massive_doc_profile_limits(
    MassiveDocDeviceProfile profile) noexcept {
    return kMassiveDocProfileLimits[static_cast<std::size_t>(profile)];
}

constexpr const MassiveDocProfileLimits* find_massive_doc_profile_limits(
    std::string_view name) noexcept {
    for (const auto& profile : kMassiveDocProfileLimits) {
        if (name == profile.name) {
            return &profile;
        }
    }
    return nullptr;
}

struct MassiveDocProfileBudgetReceipt {
    std::size_t hot_budget_bytes{0U};
    std::size_t hot_block_cache_bytes{0U};
    std::size_t hot_layout_cache_bytes{0U};
    std::size_t hot_unallocated_bytes{0U};

    std::size_t warm_budget_bytes{0U};
    std::size_t warm_block_cache_bytes{0U};
    std::size_t warm_checkpoint_cache_bytes{0U};
    std::size_t warm_source_window_cache_bytes{0U};
    std::size_t warm_unallocated_bytes{0U};

    std::size_t cold_budget_bytes{0U};
    std::size_t cold_mapped_window_bytes{0U};
    std::size_t cold_unallocated_bytes{0U};

    bool within_profile_budgets{false};
};

struct MassiveDocProfileRuntimeConfig {
    MassiveDocProfileLimits limits{};
    StoreReadConfig store_read{};
    LayoutConfig layout{};
    MassiveDocProfileBudgetReceipt budgets{};
};

constexpr std::size_t profile_mb_bytes(std::uint32_t megabytes) noexcept {
    return static_cast<std::size_t>(megabytes) * kProfileDecimalMb;
}

inline MassiveDocProfileRuntimeConfig make_massive_doc_profile_runtime_config(
    MassiveDocDeviceProfile profile) noexcept {
    MassiveDocProfileRuntimeConfig output;
    output.limits = massive_doc_profile_limits(profile);

    const std::size_t hot_budget = profile_mb_bytes(output.limits.hot_cache_mb);
    const std::size_t warm_budget = profile_mb_bytes(output.limits.warm_cache_mb);
    const std::size_t cold_budget = profile_mb_bytes(output.limits.cold_cache_mb);

    const std::size_t hot_block = std::min(kDefaultHotBlockCacheBytes, hot_budget);
    const std::size_t hot_layout = hot_budget - hot_block;

    const std::size_t warm_block = std::min(kDefaultWarmBlockCacheBytes, warm_budget);
    std::size_t warm_remaining = warm_budget - warm_block;
    const std::size_t checkpoint_cache =
        std::min(output.layout.max_checkpoint_cache_bytes, warm_remaining);
    warm_remaining -= checkpoint_cache;
    const std::size_t source_window_cache =
        std::min(output.layout.max_source_window_cache_bytes, warm_remaining);
    warm_remaining -= source_window_cache;

    const std::size_t cold_window =
        std::min(cold_budget, kMaximumColdMappedWindowBytes);

    output.store_read.block_cache.block_bytes = kDefaultImmutableBlockBytes;
    output.store_read.block_cache.hot_bytes = hot_block;
    output.store_read.block_cache.warm_bytes = warm_block;
    output.store_read.cold_window_bytes = cold_window;

    output.layout.max_cache_bytes = hot_layout;
    output.layout.max_checkpoint_cache_bytes = checkpoint_cache;
    output.layout.max_source_window_cache_bytes = source_window_cache;

    output.budgets = MassiveDocProfileBudgetReceipt{
        hot_budget,
        hot_block,
        hot_layout,
        hot_budget - hot_block - hot_layout,
        warm_budget,
        warm_block,
        checkpoint_cache,
        source_window_cache,
        warm_remaining,
        cold_budget,
        cold_window,
        cold_budget - cold_window,
        false,
    };
    output.budgets.within_profile_budgets =
        output.budgets.hot_block_cache_bytes + output.budgets.hot_layout_cache_bytes <=
            output.budgets.hot_budget_bytes &&
        output.budgets.warm_block_cache_bytes + output.budgets.warm_checkpoint_cache_bytes +
                output.budgets.warm_source_window_cache_bytes <=
            output.budgets.warm_budget_bytes &&
        output.budgets.cold_mapped_window_bytes <= output.budgets.cold_budget_bytes;
    return output;
}

} // namespace zevryon::massivedoc
