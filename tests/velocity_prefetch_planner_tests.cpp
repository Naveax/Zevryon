#include "velocity_prefetch_planner.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        fail(message);
    }
}

void test_policy_validation() {
    require(VelocityPrefetchPolicy{}.valid(), "default policy invalid");
    auto invalid = VelocityPrefetchPolicy{};
    invalid.fast_velocity_q8_per_second = invalid.medium_velocity_q8_per_second;
    require(!invalid.valid(), "equal velocity thresholds accepted");
    invalid = VelocityPrefetchPolicy{};
    invalid.max_lookahead_windows = 3U;
    require(!invalid.valid(), "lookahead cap below fast tier accepted");
    invalid = VelocityPrefetchPolicy{};
    invalid.max_lookahead_windows = 65U;
    require(!invalid.valid(), "unbounded lookahead window count accepted");
}

void test_velocity_bands_are_symmetric() {
    constexpr std::size_t kWindow = 64U * 1024U;
    const VelocityPrefetchPolicy policy;

    const auto stopped = plan_velocity_prefetch(0, kWindow, policy);
    require(stopped.band == VelocityPrefetchBand::Stationary &&
                stopped.lookahead_windows == 0U &&
                stopped.additional_lead_bytes == 0U,
            "stationary velocity scheduled speculative lead");

    const auto slow = plan_velocity_prefetch(4096, kWindow, policy);
    const auto reverse_slow = plan_velocity_prefetch(-4096, kWindow, policy);
    require(slow.band == VelocityPrefetchBand::Slow && slow.lookahead_windows == 1U,
            "slow forward velocity tier mismatch");
    require(reverse_slow.band == slow.band &&
                reverse_slow.lookahead_windows == slow.lookahead_windows,
            "velocity policy is not direction symmetric");
    require(slow.additional_lead_bytes == 0U,
            "slow tier changed existing one-window behavior");

    const auto medium = plan_velocity_prefetch(
        static_cast<std::int64_t>(policy.medium_velocity_q8_per_second),
        kWindow,
        policy);
    require(medium.band == VelocityPrefetchBand::Medium &&
                medium.lookahead_windows == 2U &&
                medium.additional_lead_bytes == kWindow,
            "medium tier lead mismatch");

    const auto fast = plan_velocity_prefetch(
        static_cast<std::int64_t>(policy.fast_velocity_q8_per_second),
        kWindow,
        policy);
    require(fast.band == VelocityPrefetchBand::Fast &&
                fast.lookahead_windows == 4U &&
                fast.additional_lead_bytes == 3U * kWindow,
            "fast tier lead mismatch");
}

void test_extreme_negative_velocity_and_saturation() {
    auto policy = VelocityPrefetchPolicy{};
    policy.fast_lookahead_windows = 64U;
    policy.max_lookahead_windows = 64U;
    const auto minimum = plan_velocity_prefetch(
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::size_t>::max(),
        policy);
    require(minimum.band == VelocityPrefetchBand::Fast,
            "INT64_MIN magnitude was not handled safely");
    require(minimum.lookahead_windows == 64U,
            "maximum bounded fast lookahead mismatch");
    require(minimum.additional_lead_bytes == std::numeric_limits<std::uint64_t>::max(),
            "lead-byte overflow did not saturate");
}

void test_invalid_inputs_fail_closed() {
    auto invalid = VelocityPrefetchPolicy{};
    invalid.slow_lookahead_windows = 0U;
    require(plan_velocity_prefetch(1000, 65536U, invalid).lookahead_windows == 0U,
            "invalid policy did not fail closed");
    require(plan_velocity_prefetch(1000, 0U, VelocityPrefetchPolicy{}).lookahead_windows == 0U,
            "zero-sized window did not fail closed");
}

} // namespace

int main() {
    test_policy_validation();
    test_velocity_bands_are_symmetric();
    test_extreme_negative_velocity_and_saturation();
    test_invalid_inputs_fail_closed();
    std::cout << "Zevryon velocity-prefetch planner tests passed\n";
    return 0;
}
