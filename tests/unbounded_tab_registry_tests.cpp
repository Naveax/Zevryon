#include "shared_source_prefetch_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;

[[noreturn]] void die(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        die(message);
    }
}

void test_default_policy_has_no_finite_tab_ceiling() {
    const SharedSourcePrefetchPoolConfig config{};
    require(config.valid(), "default shared pool configuration is invalid");
    require(
        config.max_sessions == std::numeric_limits<std::size_t>::max(),
        "default shared pool restored a finite session ceiling");

    SharedSourcePrefetchPool pool(config);
    require(pool.valid(), "default shared pool is invalid");

    // This is deliberately only a regression sample, not a product limit.
    // It is far above the historical 256-session default while remaining
    // cheap enough for Windows/Linux CI. Policy admission is SIZE_MAX.
    constexpr std::uint64_t kRegressionSampleSessions = 4096U;
    const PrefetchTicket stationary{1U, 0};
    for (std::uint64_t session_id = 1U;
         session_id <= kRegressionSampleSessions;
         ++session_id) {
        require(
            pool.open_session(session_id, {}, stationary, false),
            "unbounded tab registry rejected regression-sample session");
    }

    const SharedSourcePrefetchPoolStatus opened = pool.status();
    require(
        opened.sessions == static_cast<std::size_t>(kRegressionSampleSessions),
        "unbounded tab registry session count mismatch");
    require(opened.active_sessions == 0U,
            "inactive regression sessions were reported active");
    require(opened.live_threads == 0U,
            "registering many idle tabs started native worker threads");
    require(opened.ready_bytes == 0U && opened.ready_results == 0U,
            "registering idle tabs retained speculative payload memory");

    for (std::uint64_t session_id = 1U;
         session_id <= kRegressionSampleSessions;
         ++session_id) {
        require(pool.close_session(session_id),
                "unbounded tab registry failed to close session");
    }

    const SharedSourcePrefetchPoolStatus closed = pool.status();
    require(closed.sessions == 0U, "closed registry retained sessions");
    require(closed.live_threads == 0U,
            "close-only registry lifecycle started worker threads");
}

} // namespace

int main() {
    test_default_policy_has_no_finite_tab_ceiling();
    std::cout << "Zevryon unbounded tab-registry tests passed\n";
    return 0;
}
