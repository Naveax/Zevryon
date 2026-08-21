#include "shared_source_prefetch_pool.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;
using namespace std::chrono_literals;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        fail(message);
    }
}

SourceWindowPrefetchRequest make_request(PrefetchTicket ticket) {
    SourceWindowPrefetchRequest request;
    request.record_index = 1U;
    request.byte_offset = 0U;
    request.max_bytes = 1U;
    request.ticket = ticket;
    return request;
}

} // namespace

int main() {
    SharedSourcePrefetchPool pool(
        {1U, 8192U, 64U * 1024U},
        [](const std::filesystem::path&,
           std::uint64_t,
           const SourceWindowPrefetchRequest& request,
           std::vector<std::byte>* bytes,
           std::string* error) {
            bytes->assign(request.max_bytes, std::byte{0x2a});
            error->clear();
            return true;
        });

    const PrefetchTicket ticket{900U, 1};
    constexpr std::uint64_t kRegressionSessions = 4096U;
    for (std::uint64_t id = 0U; id < kRegressionSessions; ++id) {
        require(
            pool.open_session(id, {}, ticket, (id % 2U) == 0U),
            "4096-session telemetry registration failed");
    }

    SharedSourcePrefetchPoolStatus status = pool.status();
    require(status.sessions == 4096U, "session counter mismatch");
    require(status.active_sessions == 2048U, "active counter mismatch");
    require(status.queued_sessions == 0U && status.running_sessions == 0U &&
                status.ready_results == 0U,
            "idle telemetry counters started non-zero");

    require(pool.set_session_authority(1U, ticket, true),
            "inactive-to-active transition failed");
    status = pool.status();
    require(status.active_sessions == 2049U,
            "active transition was not reflected incrementally");

    require(
        pool.request(1U, make_request(ticket)) ==
            SharedSourcePrefetchScheduleResult::accepted,
        "telemetry lifecycle request rejected");
    require(pool.wait_idle_for(5s), "telemetry lifecycle request did not settle");
    status = pool.status();
    require(status.queued_sessions == 0U && status.running_sessions == 0U,
            "queue/running counters did not settle to zero");
    require(status.ready_results == 1U,
            "ready-result counter did not increment");

    SourceWindowPrefetchResult result;
    require(pool.try_take_ready(1U, &result), "ready result missing");
    status = pool.status();
    require(status.ready_results == 0U,
            "ready-result counter did not decrement on take");

    require(pool.close_session(1U), "active session close failed");
    status = pool.status();
    require(status.sessions == 4095U, "session close counter mismatch");
    require(status.active_sessions == 2048U,
            "active close transition counter mismatch");

    require(pool.close_session(0U), "second active session close failed");
    status = pool.status();
    require(status.sessions == 4094U && status.active_sessions == 2047U,
            "second close transition counter mismatch");

    std::cout << "Zevryon shared prefetch O(1) status counters passed\n";
    return 0;
}
