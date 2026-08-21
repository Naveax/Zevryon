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

    policy.reset();
    ZenithProcessMemorySnapshot psi = pct(900U);
    psi.psi_memory_available = true;
    psi.psi_some_avg10_q16 = 8192U;
    require(
        policy.update(psi, &pressure) && pressure == FramePressure::Elevated,
        "12.5 percent PSI some did not enter elevated pressure");
    psi.psi_some_avg10_q16 = 6100U;
    require(
        policy.update(psi, &pressure) && pressure == FramePressure::Elevated,
        "PSI elevated hysteresis released early");
    psi.psi_some_avg10_q16 = 5000U;
    require(
        policy.update(psi, &pressure) && pressure == FramePressure::Normal,
        "PSI elevated pressure did not recover");

    psi.psi_full_avg10_q16 = 1638U;
    require(
        policy.update(psi, &pressure) && pressure == FramePressure::Critical,
        "2.5 percent PSI full did not enter critical pressure");
    psi.psi_full_avg10_q16 = 800U;
    require(
        policy.update(psi, &pressure) && pressure == FramePressure::Critical,
        "PSI critical hysteresis released early");
    psi.psi_full_avg10_q16 = 500U;
    require(
        policy.update(psi, &pressure) && pressure == FramePressure::Normal,
        "PSI critical pressure did not recover");

    ZenithProcessMemorySnapshot invalid{0U, 2U, 1U};
    require(!policy.update(invalid, &pressure), "invalid snapshot accepted");
    ZenithProcessMemorySnapshot invalid_psi = pct(900U);
    invalid_psi.psi_memory_available = true;
    invalid_psi.psi_some_avg10_q16 = 65'537U;
    require(!policy.update(invalid_psi, &pressure), "invalid PSI snapshot accepted");
    require(policy.stats().invalid_samples == 2U, "invalid samples were not counted");

    std::string error;
    ZenithProcessMemorySnapshot actual;
    require(
        capture_zenith_process_memory_snapshot(&actual, &error),
        "platform memory snapshot failed");
    require(actual.valid(), "platform memory snapshot is invalid");

    std::cout << "Zevryon process memory-pressure tests passed\n";
    return 0;
}
