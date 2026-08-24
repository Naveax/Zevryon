#include "foreground_layout_handoff.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
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

ForegroundLayoutRequest request(std::uint64_t id) {
    ForegroundLayoutRequest result;
    result.request_id = id;
    result.viewport_width_q8 = 800U * 256U;
    result.viewport_height_q8 = 720U * 256U;
    result.max_fragments = 16U;
    return result;
}

ForegroundLayoutReady ready(std::uint64_t id) {
    ForegroundLayoutReady result;
    result.request_id = id;
    result.succeeded = true;
    return result;
}

} // namespace

int main() {
    SharedForegroundLayoutHandoff handoff({64U * 1024U, 32U});
    require(handoff.open_session(71U), "activity-race session open failed");
    require(
        handoff.schedule(71U, request(100U)) ==
            ForegroundLayoutScheduleResult::Accepted,
        "activity-race request not accepted");

    ForegroundLayoutRequest running;
    require(handoff.try_take_pending(71U, &running),
            "activity-race request not claimed");
    require(running.request_id == 100U,
            "activity-race worker claimed wrong request");

    require(handoff.set_session_active(71U, false),
            "activity-race deactivation failed");
    require(handoff.set_session_active(71U, true),
            "activity-race reactivation failed");

    require(!handoff.publish_ready(71U, ready(100U)),
            "pre-hide running result became authoritative after reactivation");
    require(handoff.status().stale_publications == 1U,
            "invalidated running publication was not counted stale");

    require(
        handoff.schedule(71U, request(101U)) ==
            ForegroundLayoutScheduleResult::Accepted,
        "fresh post-reactivation request not accepted");
    require(handoff.try_take_pending(71U, &running),
            "fresh post-reactivation request not claimable");
    require(running.request_id == 101U,
            "post-reactivation worker claimed stale identity");
    require(handoff.publish_ready(71U, ready(101U)),
            "fresh post-reactivation result was rejected");

    ForegroundLayoutReady published;
    require(handoff.try_take_ready(71U, &published),
            "fresh post-reactivation result not deliverable");
    require(published.request_id == 101U,
            "fresh post-reactivation identity changed");

    std::cout << "Zevryon foreground layout activity-race tests passed\n";
    return 0;
}
