#include "zenith_process_memory_pressure.hpp"

#include <cstdlib>
#include <iostream>
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

ZenithProcessMemorySnapshot windows_signal(
    std::uint64_t available,
    bool signaled) {
    ZenithProcessMemorySnapshot snapshot = pct(available);
    snapshot.windows_low_memory_notification_available = true;
    snapshot.windows_low_memory_signaled = signaled;
    return snapshot;
}

} // namespace

int main() {
    ZenithProcessMemoryPressurePolicy policy;
    require(policy.valid(), "default memory-pressure policy invalid");

    FramePressure pressure = FramePressure::Critical;
    require(
        policy.update(pct(200U), &pressure) && pressure == FramePressure::Normal,
        "20 percent available memory was not normal");
    require(
        policy.update(pct(140U), &pressure) && pressure == FramePressure::Elevated,
        "14 percent available memory was not elevated");
    require(
        policy.update(pct(160U), &pressure) && pressure == FramePressure::Elevated,
        "elevated hysteresis released early");
    require(
        policy.update(pct(190U), &pressure) && pressure == FramePressure::Normal,
        "elevated hysteresis did not recover");
    require(
        policy.update(pct(70U), &pressure) && pressure == FramePressure::Critical,
        "7 percent available memory was not critical");
    require(
        policy.update(pct(100U), &pressure) && pressure == FramePressure::Critical,
        "critical hysteresis released early");
    require(
        policy.update(pct(120U), &pressure) && pressure == FramePressure::Elevated,
        "critical pressure did not recover to elevated");
    require(
        policy.update(pct(190U), &pressure) && pressure == FramePressure::Normal,
        "pressure did not recover to normal");

    require(
        policy.update(windows_signal(200U, true), &pressure) &&
            pressure == FramePressure::Elevated,
        "Windows low-memory signal did not raise elevated pressure");
    require(
        policy.stats().windows_low_memory_samples == 1U &&
            policy.stats().windows_low_memory_escalations == 1U,
        "Windows low-memory signal telemetry changed");
    require(
        policy.update(windows_signal(200U, false), &pressure) &&
            pressure == FramePressure::Normal,
        "cleared Windows low-memory signal did not release pressure");
    require(
        policy.stats().windows_low_memory_samples == 2U &&
            policy.stats().windows_low_memory_escalations == 1U,
        "Windows low-memory clear telemetry changed");

    require(
        policy.update(windows_signal(70U, true), &pressure) &&
            pressure == FramePressure::Critical,
        "Windows signal lowered memory-required critical pressure");
    require(
        policy.stats().windows_low_memory_escalations == 1U,
        "Windows signal falsely claimed a critical-memory escalation");

    ZenithProcessMemoryPressureConfig disabled_config;
    disabled_config.windows_low_memory_elevated_floor = false;
    ZenithProcessMemoryPressurePolicy disabled_policy(disabled_config);
    require(
        disabled_policy.update(windows_signal(200U, true), &pressure) &&
            pressure == FramePressure::Normal,
        "disabled Windows low-memory floor changed pressure");
    require(
        disabled_policy.stats().windows_low_memory_samples == 0U,
        "disabled Windows low-memory floor counted signal samples");

    ZenithProcessMemorySnapshot invalid{0U, 2U, 1U};
    require(!policy.update(invalid, &pressure), "invalid snapshot accepted");
    require(policy.stats().invalid_samples == 1U, "invalid sample was not counted");

    std::string error;
    ZenithProcessMemorySnapshot actual;
    require(
        capture_zenith_process_memory_snapshot(&actual, &error),
        "platform memory snapshot failed");
    require(actual.valid(), "platform memory snapshot is invalid");

    std::cout << "Zevryon process memory-pressure tests passed\n";
    return 0;
}
