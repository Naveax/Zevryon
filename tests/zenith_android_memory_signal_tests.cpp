#include "zenith_android_memory_signal.hpp"
#include "zenith_process_memory_pressure.hpp"
#include "zenith_process_tab_controller.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
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

ZenithProcessMemorySnapshot pct(std::uint64_t available) {
    return {123U, available, 1000U};
}

} // namespace

int main() {
    ZenithAndroidMemoryDecision decision;
    require(
        evaluate_android_memory_signal(16'384U, {}, &decision) &&
            decision.profile == DeviceFrameProfile::Desktop &&
            decision.pressure == FramePressure::Normal,
        "normal Android signal did not preserve desktop profile");

    ZenithAndroidMemorySignal low_ram;
    low_ram.low_ram_device = true;
    require(
        evaluate_android_memory_signal(16'384U, low_ram, &decision) &&
            decision.profile == DeviceFrameProfile::LegacyPhone &&
            decision.pressure == FramePressure::Normal,
        "Android low-RAM device did not select conservative profile");

    ZenithAndroidMemorySignal ui_hidden;
    ui_hidden.trim_level = 20;
    require(
        evaluate_android_memory_signal(8192U, ui_hidden, &decision) &&
            decision.pressure == FramePressure::Elevated &&
            decision.ui_hidden && !decision.background_lru,
        "Android UI-hidden signal did not enter elevated pressure");

    ZenithAndroidMemorySignal background;
    background.trim_level = 40;
    require(
        evaluate_android_memory_signal(8192U, background, &decision) &&
            decision.pressure == FramePressure::Critical &&
            decision.ui_hidden && decision.background_lru,
        "Android background signal did not enter critical pressure");

    ZenithAndroidMemorySignal legacy_running_low;
    legacy_running_low.trim_level = 10;
    require(
        evaluate_android_memory_signal(8192U, legacy_running_low, &decision) &&
            decision.pressure == FramePressure::Elevated,
        "legacy Android running-low signal did not enter elevated pressure");

    ZenithAndroidMemorySignal legacy_running_critical;
    legacy_running_critical.trim_level = 15;
    require(
        evaluate_android_memory_signal(8192U, legacy_running_critical, &decision) &&
            decision.pressure == FramePressure::Critical,
        "legacy Android running-critical signal did not enter critical pressure");

    ZenithAndroidMemorySignal system_low;
    system_low.system_low_memory = true;
    require(
        evaluate_android_memory_signal(8192U, system_low, &decision) &&
            decision.pressure == FramePressure::Critical,
        "Android MemoryInfo.lowMemory did not enter critical pressure");

    ZenithAndroidMemorySignal invalid;
    invalid.trim_level = -1;
    require(
        !evaluate_android_memory_signal(8192U, invalid, &decision),
        "negative Android trim level was accepted");
    require(
        !evaluate_android_memory_signal(0U, {}, &decision),
        "zero Android RAM was accepted");

    ZenithProcessMemoryPressurePolicy process_policy;
    require(process_policy.valid(), "default process memory policy invalid");
    ZenithProcessTabController controller;
    std::string error;

    require(
        apply_android_memory_pressure_signal(
            8192U, background, &process_policy, &controller, &decision, &error) &&
            controller.global_pressure() == FramePressure::Critical,
        "Android pressure was not applied above a normal process baseline");
    require(
        apply_android_memory_pressure_signal(
            8192U, {}, &process_policy, &controller, &decision, &error) &&
            controller.global_pressure() == FramePressure::Normal,
        "Android pressure did not recover to a normal process baseline");

    FramePressure process_pressure = FramePressure::Normal;
    require(
        process_policy.update(pct(70U), &process_pressure) &&
            process_pressure == FramePressure::Critical,
        "process policy did not establish critical baseline");
    require(
        apply_android_memory_pressure_signal(
            8192U, {}, &process_policy, &controller, &decision, &error) &&
            controller.global_pressure() == FramePressure::Critical,
        "normal Android signal lowered critical process pressure");

    require(
        process_policy.update(pct(200U), &process_pressure) &&
            process_pressure == FramePressure::Normal,
        "process policy did not recover to normal baseline");
    require(
        apply_android_memory_pressure_signal(
            8192U, {}, &process_policy, &controller, &decision, &error) &&
            controller.global_pressure() == FramePressure::Normal,
        "Android bridge did not recover after process baseline cleared");

    require(
        !apply_android_memory_pressure_signal(
            8192U, {}, nullptr, &controller, &decision, &error) &&
            !error.empty(),
        "Android bridge accepted a missing process policy");

    std::cout << "Zevryon Android memory-signal tests passed\n";
    return 0;
}
