#include "foreground_layout_worker_pool.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;
using namespace std::chrono_literals;

[[noreturn]] void die(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        die(message);
    }
}

ForegroundLayoutRequest request(std::uint64_t id, std::uint64_t scroll = 0U) {
    ForegroundLayoutRequest result;
    result.request_id = id;
    result.scroll_y_q8 = scroll;
    result.viewport_width_q8 = 800U * 256U;
    result.viewport_height_q8 = 720U * 256U;
    result.overscan_q8 = 360U * 256U;
    result.max_fragments = 32U;
    return result;
}

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool first_entered{false};
    bool release_first{false};
    std::uint64_t calls{0U};
};

ForegroundLayoutExecutor gated_executor(const std::shared_ptr<Gate>& gate) {
    return [gate](
               const ForegroundLayoutRequest& request_value,
               LayoutWindowResult* result,
               bool* used_checkpoint_path,
               std::string* error) {
        if (result == nullptr || used_checkpoint_path == nullptr || error == nullptr) {
            return false;
        }
        {
            std::unique_lock<std::mutex> lock(gate->mutex);
            ++gate->calls;
            if (gate->calls == 1U) {
                gate->first_entered = true;
                gate->cv.notify_all();
                gate->cv.wait(lock, [gate] { return gate->release_first; });
            }
        }
        *result = LayoutWindowResult{};
        LayoutFragment fragment;
        fragment.record_index = request_value.request_id;
        fragment.source_record_index = request_value.request_id;
        fragment.y_q8 = request_value.scroll_y_q8;
        fragment.height_q8 = 18U * 256U;
        result->fragments.push_back(fragment);
        *used_checkpoint_path = true;
        error->clear();
        return true;
    };
}

void wait_first_entered(const std::shared_ptr<Gate>& gate) {
    std::unique_lock<std::mutex> lock(gate->mutex);
    require(
        gate->cv.wait_for(lock, 5s, [gate] { return gate->first_entered; }),
        "foreground worker did not enter gated executor");
}

void release_first(const std::shared_ptr<Gate>& gate) {
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->release_first = true;
    }
    gate->cv.notify_all();
}

void test_latest_request_requeues_after_running_work() {
    const auto gate = std::make_shared<Gate>();
    SharedForegroundLayoutWorkerPool pool(
        {2U, {256U * 1024U, 64U}});
    require(pool.valid(), "foreground worker pool config invalid");
    require(pool.status().live_threads == 0U,
            "foreground workers started before first session");
    require(pool.open_session(7U, gated_executor(gate)),
            "foreground worker session open failed");

    ForegroundLayoutWorkerPoolStatus status = pool.status();
    require(status.live_threads == 2U && status.thread_starts == 2U,
            "foreground process worker bound/start accounting mismatch");

    require(
        pool.request(7U, request(1U, 10U)) ==
            ForegroundLayoutWorkerScheduleResult::Accepted,
        "first foreground worker request not accepted");
    wait_first_entered(gate);

    require(
        pool.request(7U, request(2U, 20U)) ==
            ForegroundLayoutWorkerScheduleResult::Accepted,
        "new viewport request not accepted while older layout ran");
    release_first(gate);
    require(pool.wait_idle_for(5s),
            "foreground worker pool did not settle replacement chain");

    ForegroundLayoutReady ready;
    require(pool.try_take_ready(7U, &ready),
            "newest foreground worker result not published");
    require(ready.request_id == 2U && ready.succeeded,
            "foreground worker published stale request identity");
    require(ready.result.fragments.size() == 1U &&
                ready.result.fragments.front().record_index == 2U,
            "foreground worker ready payload did not come from newest request");
    require(!pool.try_take_ready(7U, &ready),
            "foreground worker ready payload delivered twice");

    status = pool.status();
    require(status.runs_started == 2U && status.runs_succeeded == 2U,
            "foreground worker execution accounting mismatch");
    require(status.runs_not_claimed == 0U,
            "foreground worker performed redundant empty requeue");
    require(status.handoff.stale_publications == 1U,
            "older running result was not rejected as stale");
    require(status.queued_sessions == 0U && status.running_sessions == 0U,
            "foreground worker retained queue/running state after idle");
    require(pool.close_session(7U), "foreground worker session close failed");
}

void test_activity_transition_invalidates_running_worker() {
    const auto gate = std::make_shared<Gate>();
    SharedForegroundLayoutWorkerPool pool(
        {1U, {256U * 1024U, 64U}});
    require(pool.open_session(11U, gated_executor(gate)),
            "activity worker session open failed");
    require(
        pool.request(11U, request(100U)) ==
            ForegroundLayoutWorkerScheduleResult::Accepted,
        "activity worker request not accepted");
    wait_first_entered(gate);

    require(pool.set_session_active(11U, false),
            "activity worker hide failed");
    require(pool.set_session_active(11U, true),
            "activity worker resume failed");
    release_first(gate);
    require(pool.wait_idle_for(5s),
            "invalidated activity worker did not settle");

    ForegroundLayoutReady ready;
    require(!pool.try_take_ready(11U, &ready),
            "pre-hide running worker result survived reactivation");
    require(pool.status().handoff.stale_publications == 1U,
            "pre-hide running result was not counted stale");

    require(
        pool.request(11U, request(101U)) ==
            ForegroundLayoutWorkerScheduleResult::Accepted,
        "fresh resumed foreground request not accepted");
    require(pool.wait_idle_for(5s),
            "fresh resumed foreground request did not settle");
    require(pool.try_take_ready(11U, &ready),
            "fresh resumed foreground result missing");
    require(ready.request_id == 101U,
            "fresh resumed foreground result identity mismatch");
}

void test_invalid_config_and_stop() {
    SharedForegroundLayoutWorkerPool invalid_workers(
        {65U, {64U * 1024U, 32U}});
    require(!invalid_workers.valid(),
            "foreground worker pool accepted worker count above hard bound");

    SharedForegroundLayoutWorkerPool pool(
        {1U, {64U * 1024U, 32U}});
    require(pool.valid(), "stop worker pool invalid");
    pool.stop();
    require(pool.status().stopped, "foreground worker pool stop not published");
    require(!pool.open_session(
                21U,
                [](const ForegroundLayoutRequest&,
                   LayoutWindowResult*,
                   bool*,
                   std::string*) { return true; }),
            "stopped foreground worker pool opened session");
}

} // namespace

int main() {
    test_latest_request_requeues_after_running_work();
    test_activity_transition_invalidates_running_worker();
    test_invalid_config_and_stop();
    std::cout << "Zevryon foreground layout worker pool tests passed\n";
    return 0;
}
