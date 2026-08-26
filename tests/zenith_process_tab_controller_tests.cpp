#include "zenith_process_tab_controller.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

struct Event {
    std::uint64_t session_id{0U};
    FrameVisibility visibility{FrameVisibility::Hidden};
    FramePressure pressure{FramePressure::Normal};
    std::int64_t velocity{0};
};

ZenithTabActivitySink recording_sink(
    std::uint64_t session_id,
    const std::shared_ptr<std::vector<Event>>& events,
    bool succeed = true) {
    return [session_id, events, succeed](
               FrameVisibility visibility,
               FramePressure pressure,
               std::int64_t velocity,
               std::string* error) {
        events->push_back(Event{session_id, visibility, pressure, velocity});
        if (!succeed) {
            *error = "injected sink failure";
            return false;
        }
        error->clear();
        return true;
    };
}

void test_registry_has_no_controller_cardinality_limit() {
    ZenithProcessTabController controller;
    const auto events = std::make_shared<std::vector<Event>>();
    constexpr std::uint64_t kRegressionSample = 4096U;
    std::string error;
    for (std::uint64_t id = 1U; id <= kRegressionSample; ++id) {
        require(
            controller.register_tab(
                id,
                FrameVisibility::Hidden,
                12345,
                recording_sink(id, events),
                &error),
            "process controller rejected regression-sample tab");
    }
    const auto stats = controller.stats();
    require(stats.registered_tabs == kRegressionSample,
            "process controller registry count mismatch");
    require(stats.visible_tabs == 0U && stats.hidden_tabs == kRegressionSample,
            "process controller visibility accounting mismatch");
    for (const Event& event : *events) {
        require(event.visibility == FrameVisibility::Hidden && event.velocity == 0,
                "hidden registration retained active scroll velocity");
    }
}

void test_pressure_reclaims_hidden_before_visible_and_is_idempotent() {
    ZenithProcessTabController controller;
    const auto events = std::make_shared<std::vector<Event>>();
    std::string error;
    require(controller.register_tab(1U, FrameVisibility::Visible, 777,
                                    recording_sink(1U, events), &error),
            "visible tab registration failed");
    require(controller.register_tab(2U, FrameVisibility::Hidden, 888,
                                    recording_sink(2U, events), &error),
            "hidden tab two registration failed");
    require(controller.register_tab(3U, FrameVisibility::Hidden, 999,
                                    recording_sink(3U, events), &error),
            "hidden tab three registration failed");
    events->clear();

    require(controller.set_global_pressure(FramePressure::Critical, &error),
            "critical pressure application failed");
    require(events->size() == 3U, "critical pressure application count mismatch");
    require((*events)[0].visibility == FrameVisibility::Hidden &&
            (*events)[1].visibility == FrameVisibility::Hidden &&
            (*events)[2].visibility == FrameVisibility::Visible,
            "critical pressure did not reclaim hidden tabs first");
    require((*events)[0].velocity == 0 && (*events)[1].velocity == 0,
            "hidden critical application retained scroll velocity");
    require((*events)[2].velocity == 777,
            "visible critical application lost foreground velocity");

    const std::size_t before_repeat = events->size();
    require(controller.set_global_pressure(FramePressure::Critical, &error),
            "idempotent pressure repeat failed");
    require(events->size() == before_repeat,
            "unchanged pressure redundantly retrimmed every tab");

    const auto stats = controller.stats();
    require(stats.pressure_changes == 1U, "pressure change count mismatch");
    require(stats.hidden_critical_applications == 2U,
            "hidden critical application count mismatch");
    require(stats.visible_critical_applications == 1U,
            "visible critical application count mismatch");
}

void test_pressure_sources_compose_without_clobbering() {
    ZenithProcessTabController controller;
    const auto events = std::make_shared<std::vector<Event>>();
    std::string error;
    require(controller.register_tab(4U, FrameVisibility::Hidden, 0,
                                    recording_sink(4U, events), &error),
            "pressure-source tab registration failed");
    events->clear();

    require(controller.set_global_pressure(FramePressure::Elevated, &error),
            "process-memory source did not enter elevated pressure");
    require(controller.global_pressure() == FramePressure::Elevated &&
            controller.pressure_source(ZenithProcessPressureSource::ProcessMemory) ==
                FramePressure::Elevated,
            "process-memory source state mismatch");

    require(controller.set_pressure_source(
                ZenithProcessPressureSource::PlatformMemory,
                FramePressure::Critical,
                &error),
            "platform-memory source did not enter critical pressure");
    require(controller.global_pressure() == FramePressure::Critical &&
            controller.pressure_source(ZenithProcessPressureSource::PlatformMemory) ==
                FramePressure::Critical,
            "platform-memory source did not dominate effective pressure");

    const std::size_t before_process_clear = events->size();
    require(controller.set_global_pressure(FramePressure::Normal, &error),
            "process-memory source clear failed under platform pressure");
    require(controller.global_pressure() == FramePressure::Critical &&
            controller.pressure_source(ZenithProcessPressureSource::ProcessMemory) ==
                FramePressure::Normal,
            "process-memory clear incorrectly lowered platform critical pressure");
    require(events->size() == before_process_clear,
            "source-only change redundantly reapplied unchanged effective pressure");

    require(controller.set_pressure_source(
                ZenithProcessPressureSource::PlatformMemory,
                FramePressure::Normal,
                &error),
            "platform-memory source clear failed");
    require(controller.global_pressure() == FramePressure::Normal,
            "effective pressure did not recover after both sources cleared");
    require(controller.stats().pressure_changes == 3U,
            "effective pressure transition count changed under source composition");

    const auto invalid_source = static_cast<ZenithProcessPressureSource>(99U);
    require(!controller.set_pressure_source(invalid_source, FramePressure::Critical, &error),
            "invalid pressure source was accepted");
    require(!error.empty() && controller.global_pressure() == FramePressure::Normal,
            "invalid pressure source changed effective pressure");
}

void test_activity_update_uses_current_global_pressure() {
    ZenithProcessTabController controller;
    const auto events = std::make_shared<std::vector<Event>>();
    std::string error;
    require(controller.register_tab(7U, FrameVisibility::Hidden, 0,
                                    recording_sink(7U, events), &error),
            "activity-update tab registration failed");
    require(controller.set_global_pressure(FramePressure::Critical, &error),
            "activity-update critical pressure failed");
    events->clear();

    require(controller.set_tab_activity(7U, FrameVisibility::Visible, 4321, &error),
            "hidden-to-visible activity update failed");
    require(events->size() == 1U, "activity update application count mismatch");
    require((*events)[0].visibility == FrameVisibility::Visible &&
            (*events)[0].pressure == FramePressure::Critical &&
            (*events)[0].velocity == 4321,
            "activity update did not inherit current global pressure");
}

void test_pressure_continues_after_sink_failure() {
    ZenithProcessTabController controller;
    const auto events = std::make_shared<std::vector<Event>>();
    std::string error;
    require(controller.register_tab(10U, FrameVisibility::Hidden, 0,
                                    recording_sink(10U, events), &error),
            "failure-test first registration failed");
    require(controller.register_tab(11U, FrameVisibility::Visible, 5,
                                    recording_sink(11U, events), &error),
            "failure-test second registration failed");

    require(controller.unregister_tab(10U), "failure-test unregister failed");
    const auto calls = std::make_shared<std::uint64_t>(0U);
    ZenithTabActivitySink fail_after_initial =
        [events, calls](FrameVisibility visibility, FramePressure pressure,
                        std::int64_t velocity, std::string* sink_error) {
            ++(*calls);
            events->push_back(Event{10U, visibility, pressure, velocity});
            if (*calls > 1U) {
                *sink_error = "injected pressure failure";
                return false;
            }
            sink_error->clear();
            return true;
        };
    require(controller.register_tab(10U, FrameVisibility::Hidden, 0,
                                    std::move(fail_after_initial), &error),
            "failure-test replacement registration failed");
    events->clear();

    require(!controller.set_global_pressure(FramePressure::Elevated, &error),
            "injected sink failure was not reported");
    require(!error.empty(), "injected sink failure lost diagnostic");
    require(events->size() == 2U,
            "process pressure stopped before applying remaining tabs");
    require(controller.stats().application_failures == 1U,
            "sink failure telemetry mismatch");
}

} // namespace

int main() {
    test_registry_has_no_controller_cardinality_limit();
    test_pressure_reclaims_hidden_before_visible_and_is_idempotent();
    test_pressure_sources_compose_without_clobbering();
    test_activity_update_uses_current_global_pressure();
    test_pressure_continues_after_sink_failure();
    std::cout << "Zevryon process tab-pressure controller tests passed\n";
    return 0;
}
