#include "foreground_layout_handoff.hpp"

#include <cstddef>
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

ForegroundLayoutRequest request(
    std::uint64_t id,
    std::uint64_t scroll = 0U,
    std::size_t max_fragments = 16U) {
    ForegroundLayoutRequest result;
    result.request_id = id;
    result.scroll_y_q8 = scroll;
    result.viewport_width_q8 = 800U * 256U;
    result.viewport_height_q8 = 720U * 256U;
    result.overscan_q8 = 360U * 256U;
    result.max_fragments = max_fragments;
    return result;
}

ForegroundLayoutReady ready(std::uint64_t id, std::size_t fragments = 1U) {
    ForegroundLayoutReady result;
    result.request_id = id;
    result.succeeded = true;
    result.used_checkpoint_path = true;
    result.result.fragments.resize(fragments);
    for (std::size_t index = 0U; index < fragments; ++index) {
        result.result.fragments[index].record_index = static_cast<std::uint64_t>(index);
        result.result.fragments[index].source_record_index =
            static_cast<std::uint64_t>(index);
        result.result.fragments[index].height_q8 = 18U * 256U;
    }
    return result;
}

void test_latest_request_and_stale_publication() {
    SharedForegroundLayoutHandoff handoff({64U * 1024U, 64U});
    require(handoff.valid(), "handoff config unexpectedly invalid");
    require(handoff.open_session(7U), "unable to open handoff session");
    require(!handoff.open_session(7U), "duplicate handoff session accepted");

    const ForegroundLayoutRequest first = request(1U);
    require(
        handoff.schedule(7U, first) == ForegroundLayoutScheduleResult::Accepted,
        "first foreground request not accepted");
    require(
        handoff.schedule(7U, first) == ForegroundLayoutScheduleResult::Coalesced,
        "identical foreground request not coalesced");

    ForegroundLayoutRequest conflicting = first;
    conflicting.scroll_y_q8 = 1U;
    require(
        handoff.schedule(7U, conflicting) == ForegroundLayoutScheduleResult::Invalid,
        "same request identity with different payload was accepted");

    require(
        handoff.schedule(7U, request(2U, 20U)) ==
            ForegroundLayoutScheduleResult::Replaced,
        "newer request did not replace pending request");

    ForegroundLayoutRequest running;
    require(handoff.try_take_pending(7U, &running),
            "worker could not claim latest pending request");
    require(running.request_id == 2U && running.scroll_y_q8 == 20U,
            "worker claimed wrong request after replacement");
    require(!handoff.try_take_pending(7U, &running),
            "second request ran concurrently in one session");

    require(
        handoff.schedule(7U, request(3U, 40U)) ==
            ForegroundLayoutScheduleResult::Accepted,
        "new request was not accepted while older request was running");
    require(!handoff.publish_ready(7U, ready(2U)),
            "stale running result was published after newer request arrived");

    require(handoff.try_take_pending(7U, &running),
            "newest pending request was not claimable after stale completion");
    require(running.request_id == 3U,
            "wrong request claimed after stale completion");
    require(handoff.publish_ready(7U, ready(3U, 2U)),
            "current worker result was not published");

    ForegroundLayoutReady taken;
    require(handoff.try_take_ready(7U, &taken),
            "UI could not take current ready result");
    require(taken.request_id == 3U && taken.result.fragments.size() == 2U,
            "ready result identity or payload changed");
    require(!handoff.try_take_ready(7U, &taken),
            "ready result was delivered more than once");

    const ForegroundLayoutHandoffStatus status = handoff.status();
    require(status.requests_accepted == 2U,
            "accepted request accounting mismatch");
    require(status.requests_coalesced == 1U,
            "coalesced request accounting mismatch");
    require(status.requests_replaced == 1U,
            "replaced request accounting mismatch");
    require(status.requests_invalid == 1U,
            "invalid identity-reuse accounting mismatch");
    require(status.pending_invalidations == 1U,
            "pending invalidation accounting mismatch");
    require(status.stale_publications == 1U,
            "stale publication accounting mismatch");
    require(status.ready_results == 0U && status.ready_bytes == 0U,
            "ready memory was retained after delivery");
}

void test_activity_invalidation_and_stale_requests() {
    SharedForegroundLayoutHandoff handoff({64U * 1024U, 32U});
    require(handoff.open_session(11U), "activity session open failed");

    require(
        handoff.schedule(11U, request(10U)) ==
            ForegroundLayoutScheduleResult::Accepted,
        "activity baseline request not accepted");
    ForegroundLayoutRequest running;
    require(handoff.try_take_pending(11U, &running),
            "activity baseline request not claimable");
    require(handoff.publish_ready(11U, ready(10U)),
            "activity baseline result not published");
    require(handoff.status().ready_results == 1U,
            "activity baseline ready result missing");

    require(handoff.set_session_active(11U, false),
            "unable to deactivate handoff session");
    ForegroundLayoutHandoffStatus status = handoff.status();
    require(status.active_sessions == 0U && status.ready_results == 0U,
            "deactivation did not invalidate retained ready result");
    require(status.ready_invalidations == 1U,
            "ready invalidation was not counted");
    require(
        handoff.schedule(11U, request(11U)) ==
            ForegroundLayoutScheduleResult::Inactive,
        "inactive session accepted foreground work");

    require(handoff.set_session_active(11U, true),
            "unable to reactivate handoff session");
    require(
        handoff.schedule(11U, request(11U)) ==
            ForegroundLayoutScheduleResult::Accepted,
        "reactivated session did not accept fresh work");
    require(
        handoff.schedule(11U, request(9U)) ==
            ForegroundLayoutScheduleResult::Stale,
        "older request identity did not fail closed");

    status = handoff.status();
    require(status.requests_inactive == 1U,
            "inactive request accounting mismatch");
    require(status.requests_stale == 1U,
            "stale request accounting mismatch");
}

void test_global_ready_budget_fails_closed() {
    SharedForegroundLayoutHandoff handoff({128U, 64U});
    require(handoff.open_session(21U), "budget session open failed");
    require(
        handoff.schedule(21U, request(1U)) ==
            ForegroundLayoutScheduleResult::Accepted,
        "budget request not accepted");
    ForegroundLayoutRequest running;
    require(handoff.try_take_pending(21U, &running),
            "budget request not claimable");
    require(!handoff.publish_ready(21U, ready(1U, 8U)),
            "oversized ready payload exceeded global ready budget");

    const ForegroundLayoutHandoffStatus status = handoff.status();
    require(status.ready_budget_drops == 1U,
            "ready budget drop not counted");
    require(status.ready_results == 0U && status.ready_bytes == 0U,
            "dropped ready payload consumed retained ready budget");
    require(status.ready_peak_bytes == 0U,
            "dropped ready payload changed ready peak bytes");
}

void test_stop_and_close_contract() {
    SharedForegroundLayoutHandoff handoff({64U * 1024U, 32U});
    require(handoff.open_session(31U), "stop session open failed");
    require(
        handoff.schedule(31U, request(1U)) ==
            ForegroundLayoutScheduleResult::Accepted,
        "stop baseline request not accepted");
    handoff.stop();
    ForegroundLayoutHandoffStatus status = handoff.status();
    require(status.stopped, "stop state not published");
    require(status.pending_sessions == 0U && status.ready_results == 0U,
            "stop retained pending or ready work");
    require(
        handoff.schedule(31U, request(2U)) ==
            ForegroundLayoutScheduleResult::Stopped,
        "stopped handoff accepted request");
    require(!handoff.open_session(32U),
            "stopped handoff accepted new session");

    SharedForegroundLayoutHandoff closable({64U * 1024U, 32U});
    require(closable.open_session(41U), "close session open failed");
    require(closable.close_session(41U), "close session failed");
    require(!closable.close_session(41U), "closed session closed twice");
    require(
        closable.schedule(41U, request(1U)) ==
            ForegroundLayoutScheduleResult::UnknownSession,
        "closed session remained schedulable");
}

} // namespace

int main() {
    test_latest_request_and_stale_publication();
    test_activity_invalidation_and_stale_requests();
    test_global_ready_budget_fails_closed();
    test_stop_and_close_contract();
    std::cout << "Zevryon foreground layout handoff tests passed\n";
    return 0;
}
