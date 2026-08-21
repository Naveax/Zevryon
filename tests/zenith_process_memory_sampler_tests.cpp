#include "zenith_process_memory_sampler.hpp"

#include "zenith_process_tab_controller.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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

ZenithProcessMemorySnapshot snapshot_at_percent(std::uint64_t available_percent) {
    return ZenithProcessMemorySnapshot{
        64U * 1024U * 1024U,
        available_percent,
        100U};
}

void test_adaptive_cadence_and_controller_application() {
    const std::vector<ZenithProcessMemorySnapshot> sequence{
        snapshot_at_percent(20U),
        snapshot_at_percent(14U),
        snapshot_at_percent(7U),
        snapshot_at_percent(20U),
    };
    std::size_t capture_index = 0U;
    ZenithProcessMemorySampler sampler(
        {},
        [&sequence, &capture_index](
            ZenithProcessMemorySnapshot* snapshot,
            std::string* error) {
            if (snapshot == nullptr || error == nullptr ||
                capture_index >= sequence.size()) {
                return false;
            }
            *snapshot = sequence[capture_index++];
            error->clear();
            return true;
        });
    require(sampler.valid(), "default process memory sampler is invalid");

    ZenithProcessMemoryPressurePolicy policy;
    ZenithProcessTabController controller;
    std::vector<FramePressure> applied;
    std::string error;
    require(
        controller.register_tab(
            1U,
            FrameVisibility::Visible,
            1024,
            [&applied](
                FrameVisibility,
                FramePressure pressure,
                std::int64_t,
                std::string* sink_error) {
                applied.push_back(pressure);
                sink_error->clear();
                return true;
            },
            &error),
        "memory sampler controller registration failed");
    applied.clear();

    require(
        sampler.poll(0U, &policy, &controller, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Sampled,
        "initial process memory poll did not sample immediately");
    require(controller.global_pressure() == FramePressure::Normal,
            "normal snapshot changed controller pressure");
    require(sampler.stats().next_due_monotonic_ms == 1'000U,
            "normal cadence next-due mismatch");

    require(
        sampler.poll(999U, &policy, &controller, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Throttled,
        "normal cadence did not throttle early event-loop tick");
    require(capture_index == 1U,
            "throttled normal poll touched snapshot provider");

    require(
        sampler.poll(1'000U, &policy, &controller, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Sampled,
        "elevated due-time poll failed");
    require(controller.global_pressure() == FramePressure::Elevated,
            "14 percent headroom did not enter elevated pressure");
    require(sampler.stats().next_due_monotonic_ms == 1'250U,
            "elevated cadence next-due mismatch");

    require(
        sampler.poll(1'249U, &policy, &controller, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Throttled,
        "elevated cadence did not throttle early tick");

    require(
        sampler.poll(1'250U, &policy, &controller, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Sampled,
        "critical due-time poll failed");
    require(controller.global_pressure() == FramePressure::Critical,
            "7 percent headroom did not enter critical pressure");
    require(sampler.stats().next_due_monotonic_ms == 1'350U,
            "critical cadence next-due mismatch");

    require(
        sampler.poll(1'349U, &policy, &controller, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Throttled,
        "critical cadence did not throttle early tick");

    ZenithProcessMemorySnapshot captured;
    require(
        sampler.poll(1'350U, &policy, &controller, &captured, &error) ==
            ZenithProcessMemoryPollResult::Sampled,
        "critical recovery poll failed");
    require(captured.system_available_bytes == 20U,
            "sampler did not expose captured snapshot");
    require(controller.global_pressure() == FramePressure::Normal,
            "20 percent headroom did not restore normal pressure");
    require(sampler.stats().next_due_monotonic_ms == 2'350U,
            "recovered normal cadence next-due mismatch");

    const ZenithProcessMemorySamplerStats stats = sampler.stats();
    require(stats.polls == 7U && stats.samples == 4U &&
                stats.throttled == 3U && stats.failures == 0U,
            "adaptive cadence telemetry mismatch");
    require(capture_index == sequence.size(),
            "adaptive cadence snapshot count mismatch");
    require(applied.size() == 3U &&
                applied[0] == FramePressure::Elevated &&
                applied[1] == FramePressure::Critical &&
                applied[2] == FramePressure::Normal,
            "sampler/controller pressure application sequence mismatch");
}

void test_capture_failure_is_backed_off() {
    std::uint64_t capture_calls = 0U;
    ZenithProcessMemorySampler sampler(
        {},
        [&capture_calls](
            ZenithProcessMemorySnapshot*,
            std::string* error) {
            ++capture_calls;
            *error = "injected capture failure";
            return false;
        });
    ZenithProcessMemoryPressurePolicy policy;
    ZenithProcessTabController controller;
    std::string error;

    require(
        sampler.poll(0U, &policy, &controller, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Failed,
        "capture failure was not reported");
    require(error == "injected capture failure",
            "capture failure diagnostic was lost");
    require(sampler.stats().next_due_monotonic_ms == 1'000U,
            "failed normal capture was not backed off");

    require(
        sampler.poll(999U, &policy, &controller, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Throttled,
        "failed capture entered tight retry loop");
    require(capture_calls == 1U,
            "throttled failure path touched snapshot provider");

    require(
        sampler.poll(1'000U, &policy, &controller, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Failed,
        "due retry after capture failure did not execute");
    require(capture_calls == 2U,
            "capture failure retry count mismatch");
    require(sampler.stats().failures == 2U,
            "capture failure telemetry mismatch");
}

void test_invalid_config_and_reset() {
    require(
        !ZenithProcessMemorySamplerConfig{100U, 250U, 10U}.valid(),
        "sampler accepted elevated interval slower than normal interval");
    require(
        !ZenithProcessMemorySamplerConfig{1'000U, 250U, 0U}.valid(),
        "sampler accepted zero critical interval");

    std::uint64_t capture_calls = 0U;
    ZenithProcessMemorySampler sampler(
        {},
        [&capture_calls](
            ZenithProcessMemorySnapshot* snapshot,
            std::string* error) {
            ++capture_calls;
            *snapshot = snapshot_at_percent(20U);
            error->clear();
            return true;
        });
    ZenithProcessMemoryPressurePolicy policy;
    ZenithProcessTabController controller;
    std::string error;
    require(
        sampler.poll(10U, &policy, &controller, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Sampled,
        "reset test initial sample failed");
    require(
        sampler.poll(11U, &policy, &controller, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Throttled,
        "reset test did not throttle");
    sampler.reset();
    require(
        sampler.poll(11U, &policy, &controller, nullptr, &error) ==
            ZenithProcessMemoryPollResult::Sampled,
        "reset did not permit immediate fresh sample");
    require(capture_calls == 2U,
            "reset sample provider count mismatch");
}

} // namespace

int main() {
    test_adaptive_cadence_and_controller_application();
    test_capture_failure_is_backed_off();
    test_invalid_config_and_reset();
    std::cout << "Zevryon process memory sampler tests passed\n";
    return 0;
}
