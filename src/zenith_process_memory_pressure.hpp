#pragma once

#include "frame_budget_scheduler.hpp"

#include <cstdint>
#include <string>

namespace zevryon::massivedoc {

enum class ZenithProcessMemoryDomain : std::uint8_t {
    Host = 0U,
    CgroupV2,
};

struct ZenithProcessMemorySnapshot {
    std::uint64_t process_rss_bytes{0U};
    std::uint64_t system_available_bytes{0U};
    std::uint64_t system_total_bytes{0U};
    ZenithProcessMemoryDomain memory_domain{ZenithProcessMemoryDomain::Host};
    bool cgroup_v2_detected{false};
    bool cgroup_v2_limited{false};
    bool psi_available{false};
    std::uint32_t psi_some_avg10_milli_percent{0U};
    std::uint32_t psi_full_avg10_milli_percent{0U};
    bool windows_low_memory_notification_available{false};
    bool windows_low_memory_signaled{false};
    bool windows_job_detected{false};
    std::uint32_t windows_job_active_processes{0U};
    bool windows_process_memory_limit_enabled{false};
    std::uint64_t windows_process_memory_limit_bytes{0U};
    std::uint64_t windows_private_commit_bytes{0U};
    bool windows_job_memory_limit_enabled{false};
    std::uint64_t windows_job_memory_limit_bytes{0U};
    std::uint64_t windows_peak_process_memory_used_bytes{0U};
    std::uint64_t windows_peak_job_memory_used_bytes{0U};

    bool valid() const noexcept;
};

struct ZenithLinuxPsiPressureConfig {
    // Disabled by default: production thresholds require explicit calibration.
    bool enabled{false};
    std::uint32_t elevated_some_avg10_milli_percent{0U};
    std::uint32_t critical_some_avg10_milli_percent{0U};
    std::uint32_t critical_full_avg10_milli_percent{0U};

    bool valid() const noexcept;
};

struct ZenithProcessMemoryPressureConfig {
    // Q16 fractions of total physical/effective memory. Defaults: 15%, 8%, 3%.
    std::uint32_t elevated_enter_available_q16{9'830U};
    std::uint32_t critical_enter_available_q16{5'243U};
    std::uint32_t recovery_hysteresis_q16{1'966U};
    ZenithLinuxPsiPressureConfig linux_psi{};

    bool valid() const noexcept;
};

struct ZenithProcessMemoryPressureStats {
    std::uint64_t samples{0U};
    std::uint64_t invalid_samples{0U};
    std::uint64_t pressure_changes{0U};
    std::uint64_t psi_samples{0U};
    std::uint64_t psi_pressure_escalations{0U};
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
