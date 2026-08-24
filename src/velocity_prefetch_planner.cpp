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

bool sign_matches(std::int8_t direction, std::int64_t velocity) noexcept {
    return (direction > 0 && velocity > 0) || (direction < 0 && velocity < 0);
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

bool choose_velocity_prefetch_offset(
    std::int8_t direction,
    std::int64_t velocity_q8_per_second,
    std::uint64_t source_start,
    std::uint64_t source_end,
    std::size_t window_bytes,
    const VelocityPrefetchPolicy& policy,
    std::uint64_t* source_offset,
    VelocityPrefetchDecision* decision) noexcept {
    if (source_offset == nullptr || decision == nullptr || direction == 0 ||
        !sign_matches(direction, velocity_q8_per_second)) {
        return false;
    }
    *decision = plan_velocity_prefetch(
        velocity_q8_per_second,
        window_bytes,
        policy);
    if (decision->lookahead_windows == 0U) {
        return false;
    }

    if (direction > 0) {
        if (decision->additional_lead_bytes >
            std::numeric_limits<std::uint64_t>::max() - source_end) {
            return false;
        }
        *source_offset = source_end + decision->additional_lead_bytes;
        return true;
    }

    if (source_start == 0U) {
        return false;
    }
    const std::uint64_t window = static_cast<std::uint64_t>(window_bytes);
    const std::uint64_t distance =
        decision->additional_lead_bytes >
                std::numeric_limits<std::uint64_t>::max() - window
            ? std::numeric_limits<std::uint64_t>::max()
            : window + decision->additional_lead_bytes;
    *source_offset = source_start > distance ? source_start - distance : 0U;
    return *source_offset < source_start;
}

} // namespace zevryon::massivedoc
