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

ZenithProcessMemorySnapshot psi_pct(
    std::uint64_t available,
    std::uint32_t some,
    std::uint32_t full) {
    ZenithProcessMemorySnapshot snapshot = pct(available);
    snapshot.psi_available = true;
    snapshot.psi_some_avg10_milli_percent = some;
    snapshot.psi_full_avg10_milli_percent = full;
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

    ZenithProcessMemorySnapshot default_psi = psi_pct(200U, 90'000U, 90'000U);
    require(
        policy.update(default_psi, &pressure) && pressure == FramePressure::Normal,
        "default disabled PSI policy changed pressure");
    require(policy.stats().psi_samples == 0U, "disabled PSI policy counted samples");

    ZenithProcessMemoryPressureConfig invalid_enabled;
    invalid_enabled.linux_psi.enabled = true;
    require(!invalid_enabled.valid(), "uncalibrated enabled PSI policy was accepted");

    ZenithProcessMemoryPressureConfig invalid_disabled;
    invalid_disabled.linux_psi.elevated_some_avg10_milli_percent = 1U;
    require(!invalid_disabled.valid(), "disabled PSI policy accepted hidden thresholds");

    ZenithProcessMemoryPressureConfig psi_config;
    psi_config.linux_psi.enabled = true;
    psi_config.linux_psi.elevated_some_avg10_milli_percent = 5'000U;
    psi_config.linux_psi.critical_some_avg10_milli_percent = 20'000U;
    psi_config.linux_psi.critical_full_avg10_milli_percent = 1'000U;
    ZenithProcessMemoryPressurePolicy psi_policy(psi_config);
    require(psi_policy.valid(), "explicit calibrated PSI policy invalid");

    require(
        psi_policy.update(psi_pct(200U, 4'999U, 999U), &pressure) &&
            pressure == FramePressure::Normal,
        "sub-threshold PSI changed normal pressure");
    require(
        psi_policy.update(psi_pct(200U, 5'000U, 0U), &pressure) &&
            pressure == FramePressure::Elevated,
        "PSI some threshold did not raise elevated pressure");
    psi_policy.reset();
    require(
        psi_policy.update(psi_pct(200U, 20'000U, 0U), &pressure) &&
            pressure == FramePressure::Critical,
        "PSI critical some threshold did not raise critical pressure");
    psi_policy.reset();
    require(
        psi_policy.update(psi_pct(200U, 0U, 1'000U), &pressure) &&
            pressure == FramePressure::Critical,
        "PSI full threshold did not raise critical pressure");
    require(
        psi_policy.stats().psi_samples == 1U &&
            psi_policy.stats().psi_pressure_escalations == 1U,
        "PSI escalation statistics changed after reset");

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
