#pragma once

#include "frame_budget_scheduler.hpp"

#include <cstdint>
#include <string>

namespace zevryon::massivedoc {

struct ZenithProcessMemorySnapshot {
    std::uint64_t process_rss_bytes{0U};
    // Effective memory scope available to this process. On Linux this is
    // clamped to a finite cgroup-v2 memory.max headroom when present.
    std::uint64_t system_available_bytes{0U};
    std::uint64_t system_total_bytes{0U};
    bool cgroup_v2_limited{false};
    bool psi_memory_available{false};
    std::uint32_t psi_some_avg10_q16{0U};
    std::uint32_t psi_full_avg10_q16{0U};

    bool valid() const noexcept;
};

struct ZenithProcessMemoryPressureConfig {
    // Q16 fractions of total effective memory. Defaults: 15%, 8%, 3%.
    std::uint32_t elevated_enter_available_q16{9'830U};
    std::uint32_t critical_enter_available_q16{5'243U};
    std::uint32_t recovery_hysteresis_q16{1'966U};

    // Linux PSI avg10 percentages encoded as Q16 fractions of 100%.
    // Defaults: some >= 10% enters Elevated, full >= 2% enters Critical,
    // with a 1% release hysteresis to avoid pressure-state flapping.
    std::uint32_t psi_some_elevated_avg10_q16{6'554U};
    std::uint32_t psi_full_critical_avg10_q16{1'311U};
    std::uint32_t psi_recovery_hysteresis_q16{655U};

    bool valid() const noexcept;
};

struct ZenithProcessMemoryPressureStats {
    std::uint64_t samples{0U};
    std::uint64_t invalid_samples{0U};
    std::uint64_t pressure_changes{0U};
    FramePressure pressure{FramePressure::Normal};
};

class ZenithProcessMemoryPressurePolicy final {
public:
    explicit ZenithProcessMemoryPressurePolicy(
        ZenithProcessMemoryPressureConfig config = {});

    bool valid() const noexcept;
    bool update(
        const ZenithProcessMemorySnapshot& snapshot,
        FramePressure* pressure) noexcept;
    FramePressure pressure() const noexcept;
    ZenithProcessMemoryPressureStats stats() const noexcept;
    void reset() noexcept;

private:
    ZenithProcessMemoryPressureConfig config_;
    FramePressure pressure_{FramePressure::Normal};
    ZenithProcessMemoryPressureStats stats_;
};

bool capture_zenith_process_memory_snapshot(
    ZenithProcessMemorySnapshot* snapshot,
    std::string* error);

class ZenithProcessTabController;

bool apply_zenith_process_memory_pressure_snapshot(
    ZenithProcessMemoryPressurePolicy* policy,
    ZenithProcessTabController* controller,
    const ZenithProcessMemorySnapshot& snapshot,
    std::string* error);

bool sample_and_apply_zenith_process_memory_pressure(
    ZenithProcessMemoryPressurePolicy* policy,
    ZenithProcessTabController* controller,
    ZenithProcessMemorySnapshot* captured,
    std::string* error);

} // namespace zevryon::massivedoc
