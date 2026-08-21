#include "velocity_prefetch_planner.hpp"

#include <limits>

namespace zevryon::massivedoc {
namespace {

std::uint64_t magnitude(std::int64_t value) noexcept {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }
    return static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

std::uint64_t saturating_multiply(
    std::uint64_t left,
    std::uint64_t right) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

} // namespace

bool VelocityPrefetchPolicy::valid() const noexcept {
    return medium_velocity_q8_per_second > 0U &&
           fast_velocity_q8_per_second > medium_velocity_q8_per_second &&
           slow_lookahead_windows > 0U &&
           medium_lookahead_windows >= slow_lookahead_windows &&
           fast_lookahead_windows >= medium_lookahead_windows &&
           max_lookahead_windows >= fast_lookahead_windows &&
           max_lookahead_windows <= 64U;
}

VelocityPrefetchDecision plan_velocity_prefetch(
    std::int64_t velocity_q8_per_second,
    std::size_t window_bytes,
    const VelocityPrefetchPolicy& policy) noexcept {
    VelocityPrefetchDecision decision;
    if (!policy.valid() || velocity_q8_per_second == 0 || window_bytes == 0U) {
        return decision;
    }

    const std::uint64_t absolute_velocity = magnitude(velocity_q8_per_second);
    if (absolute_velocity >= policy.fast_velocity_q8_per_second) {
        decision.band = VelocityPrefetchBand::Fast;
        decision.lookahead_windows = policy.fast_lookahead_windows;
    } else if (absolute_velocity >= policy.medium_velocity_q8_per_second) {
        decision.band = VelocityPrefetchBand::Medium;
        decision.lookahead_windows = policy.medium_lookahead_windows;
    } else {
        decision.band = VelocityPrefetchBand::Slow;
        decision.lookahead_windows = policy.slow_lookahead_windows;
    }

    const std::uint64_t extra_windows = decision.lookahead_windows > 0U
                                            ? decision.lookahead_windows - 1U
                                            : 0U;
    decision.additional_lead_bytes = saturating_multiply(
        extra_windows,
        static_cast<std::uint64_t>(window_bytes));
    return decision;
}

} // namespace zevryon::massivedoc
