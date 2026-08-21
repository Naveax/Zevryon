#pragma once

#include <cstddef>
#include <cstdint>

namespace zevryon::massivedoc {

enum class VelocityPrefetchBand : std::uint8_t {
    Stationary = 0U,
    Slow,
    Medium,
    Fast,
};

struct VelocityPrefetchPolicy {
    std::uint64_t medium_velocity_q8_per_second{128U * 256U};
    std::uint64_t fast_velocity_q8_per_second{512U * 256U};
    std::uint32_t slow_lookahead_windows{1U};
    std::uint32_t medium_lookahead_windows{2U};
    std::uint32_t fast_lookahead_windows{4U};
    std::uint32_t max_lookahead_windows{8U};

    bool valid() const noexcept;
};

struct VelocityPrefetchDecision {
    VelocityPrefetchBand band{VelocityPrefetchBand::Stationary};
    std::uint32_t lookahead_windows{0U};
    std::uint64_t additional_lead_bytes{0U};
};

VelocityPrefetchDecision plan_velocity_prefetch(
    std::int64_t velocity_q8_per_second,
    std::size_t window_bytes,
    const VelocityPrefetchPolicy& policy = {}) noexcept;

bool choose_velocity_prefetch_offset(
    std::int8_t direction,
    std::int64_t velocity_q8_per_second,
    std::uint64_t source_start,
    std::uint64_t source_end,
    std::size_t window_bytes,
    const VelocityPrefetchPolicy& policy,
    std::uint64_t* source_offset,
    VelocityPrefetchDecision* decision) noexcept;

} // namespace zevryon::massivedoc
