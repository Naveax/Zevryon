#include "zenith_android_memory_signal.hpp"
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

    ZenithProcessTabController controller;
    std::string error;

    require(
        apply_android_memory_pressure_signal(
            8192U, background, &controller, &decision, &error) &&
            controller.pressure_source(ZenithProcessPressureSource::PlatformMemory) ==
                FramePressure::Critical &&
            controller.global_pressure() == FramePressure::Critical,
        "Android pressure was not stored in the platform source");

    require(
        controller.set_global_pressure(FramePressure::Elevated, &error) &&
            controller.pressure_source(ZenithProcessPressureSource::ProcessMemory) ==
                FramePressure::Elevated &&
            controller.global_pressure() == FramePressure::Critical,
        "weaker process pressure lowered active Android critical pressure");

    require(
        controller.set_global_pressure(FramePressure::Normal, &error) &&
            controller.global_pressure() == FramePressure::Critical,
        "process recovery cleared active Android critical pressure");

    require(
        apply_android_memory_pressure_signal(
            8192U, {}, &controller, &decision, &error) &&
            controller.pressure_source(ZenithProcessPressureSource::PlatformMemory) ==
                FramePressure::Normal &&
            controller.global_pressure() == FramePressure::Normal,
        "Android recovery did not clear its own platform pressure source");

    require(
        controller.set_global_pressure(FramePressure::Critical, &error) &&
            controller.global_pressure() == FramePressure::Critical,
        "process source did not establish critical pressure");
    require(
        apply_android_memory_pressure_signal(
            8192U, ui_hidden, &controller, &decision, &error) &&
            controller.pressure_source(ZenithProcessPressureSource::PlatformMemory) ==
                FramePressure::Elevated &&
            controller.global_pressure() == FramePressure::Critical,
        "Android elevated signal lowered active process critical pressure");
    require(
        apply_android_memory_pressure_signal(
            8192U, {}, &controller, &decision, &error) &&
            controller.global_pressure() == FramePressure::Critical,
        "normal Android signal cleared active process critical pressure");
    require(
        controller.set_global_pressure(FramePressure::Normal, &error) &&
            controller.global_pressure() == FramePressure::Normal,
        "process recovery did not restore normal pressure after Android cleared");

    require(
        !apply_android_memory_pressure_signal(
            8192U, {}, nullptr, &decision, &error) &&
            !error.empty(),
        "Android bridge accepted a missing controller");
    require(
        !apply_android_memory_pressure_signal(
            8192U, {}, &controller, &decision, nullptr),
        "Android bridge accepted a missing error output");

    std::cout << "Zevryon Android memory-signal tests passed\n";
    return 0;
}
