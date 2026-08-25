#include "zenith_process_memory_pressure.hpp"

#include "zenith_linux_memory_context.hpp"
#include "zenith_process_tab_controller.hpp"
#include "zenith_windows_memory_context.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace zevryon::massivedoc {
namespace {

constexpr std::uint64_t kQ16One = 65'536U;
constexpr std::uint32_t kMaxPsiMilliPercent = 100U * 1000U;

std::uint64_t saturating_increment(std::uint64_t value) noexcept {
    return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1U;
}

std::uint32_t available_q16(const ZenithProcessMemorySnapshot& snapshot) noexcept {
    if (!snapshot.valid()) {
        return 0U;
    }
    const std::uint64_t scaled_limit =
        std::numeric_limits<std::uint64_t>::max() / kQ16One;
    if (snapshot.system_available_bytes <= scaled_limit) {
        return static_cast<std::uint32_t>(
            (snapshot.system_available_bytes * kQ16One) /
            snapshot.system_total_bytes);
    }
    const long double ratio =
        static_cast<long double>(snapshot.system_available_bytes) /
        static_cast<long double>(snapshot.system_total_bytes);
    const long double scaled = ratio * static_cast<long double>(kQ16One);
    return static_cast<std::uint32_t>(std::min<long double>(scaled, kQ16One));
}

unsigned int pressure_rank(FramePressure pressure) noexcept {
    switch (pressure) {
    case FramePressure::Critical:
        return 2U;
    case FramePressure::Elevated:
        return 1U;
    case FramePressure::Normal:
    default:
        return 0U;
    }
}

FramePressure higher_pressure(FramePressure left, FramePressure right) noexcept {
    return pressure_rank(left) >= pressure_rank(right) ? left : right;
}

FramePressure linux_psi_pressure_floor(
    const ZenithProcessMemorySnapshot& snapshot,
    const ZenithLinuxPsiPressureConfig& config) noexcept {
    if (!config.enabled || !snapshot.psi_available) {
        return FramePressure::Normal;
    }
    if (snapshot.psi_full_avg10_milli_percent >=
            config.critical_full_avg10_milli_percent ||
        snapshot.psi_some_avg10_milli_percent >=
            config.critical_some_avg10_milli_percent) {
        return FramePressure::Critical;
    }
    if (snapshot.psi_some_avg10_milli_percent >=
        config.elevated_some_avg10_milli_percent) {
        return FramePressure::Elevated;
    }
    return FramePressure::Normal;
}

#if defined(__linux__)
bool linux_meminfo_value_kib(const char* key, std::uint64_t* value) {
    std::ifstream input("/proc/meminfo");
    if (!input) {
        return false;
    }
    std::string name;
    std::uint64_t amount = 0U;
    std::string unit;
    while (input >> name >> amount >> unit) {
        if (!name.empty() && name.back() == ':') {
            name.pop_back();
        }
        if (name == key) {
            *value = amount;
            return true;
        }
    }
    return false;
}
#endif

} // namespace

bool ZenithProcessMemorySnapshot::valid() const noexcept {
    if (system_total_bytes == 0U || system_available_bytes > system_total_bytes) {
        return false;
    }
    if (psi_available &&
        (psi_some_avg10_milli_percent > kMaxPsiMilliPercent ||
         psi_full_avg10_milli_percent > kMaxPsiMilliPercent)) {
        return false;
    }
    if (memory_domain == ZenithProcessMemoryDomain::CgroupV2 &&
        (!cgroup_v2_detected || !cgroup_v2_limited)) {
        return false;
    }
    if (memory_domain == ZenithProcessMemoryDomain::Host && cgroup_v2_limited) {
        return false;
    }
    if (windows_low_memory_signaled &&
        !windows_low_memory_notification_available) {
        return false;
    }
    if (windows_process_memory_limit_enabled &&
        (!windows_job_detected || windows_process_memory_limit_bytes == 0U)) {
        return false;
    }
    if (windows_job_memory_limit_enabled &&
        (!windows_job_detected || windows_job_memory_limit_bytes == 0U)) {
        return false;
    }
    if (!windows_process_memory_limit_enabled &&
        windows_process_memory_limit_bytes != 0U) {
        return false;
    }
    if (!windows_job_memory_limit_enabled && windows_job_memory_limit_bytes != 0U) {
        return false;
    }
    return true;
}

bool ZenithLinuxPsiPressureConfig::valid() const noexcept {
    if (!enabled) {
        return elevated_some_avg10_milli_percent == 0U &&
               critical_some_avg10_milli_percent == 0U &&
               critical_full_avg10_milli_percent == 0U;
    }
    return elevated_some_avg10_milli_percent > 0U &&
           elevated_some_avg10_milli_percent <= kMaxPsiMilliPercent &&
           critical_some_avg10_milli_percent >=
               elevated_some_avg10_milli_percent &&
           critical_some_avg10_milli_percent <= kMaxPsiMilliPercent &&
           critical_full_avg10_milli_percent > 0U &&
           critical_full_avg10_milli_percent <= kMaxPsiMilliPercent;
}

bool ZenithProcessMemoryPressureConfig::valid() const noexcept {
    return critical_enter_available_q16 > 0U &&
           critical_enter_available_q16 < elevated_enter_available_q16 &&
           elevated_enter_available_q16 < kQ16One &&
           recovery_hysteresis_q16 > 0U &&
           recovery_hysteresis_q16 < kQ16One &&
           elevated_enter_available_q16 <=
               kQ16One - recovery_hysteresis_q16 - 1U &&
           critical_enter_available_q16 <=
               elevated_enter_available_q16 - recovery_hysteresis_q16 - 1U &&
           linux_psi.valid();
}

ZenithProcessMemoryPressurePolicy::ZenithProcessMemoryPressurePolicy(
    ZenithProcessMemoryPressureConfig config)
    : config_(config) {}

bool ZenithProcessMemoryPressurePolicy::valid() const noexcept {
    return config_.valid();
}

bool ZenithProcessMemoryPressurePolicy::update(
    const ZenithProcessMemorySnapshot& snapshot,
    FramePressure* pressure) noexcept {
    stats_.samples = saturating_increment(stats_.samples);
    if (!valid() || !snapshot.valid() || pressure == nullptr) {
        stats_.invalid_samples = saturating_increment(stats_.invalid_samples);
        return false;
    }

    const std::uint32_t available = available_q16(snapshot);
    FramePressure next = pressure_;
    if (available <= config_.critical_enter_available_q16) {
        next = FramePressure::Critical;
    } else if (pressure_ == FramePressure::Critical &&
               available <= config_.critical_enter_available_q16 +
                                config_.recovery_hysteresis_q16) {
        next = FramePressure::Critical;
    } else if (available <= config_.elevated_enter_available_q16) {
        next = FramePressure::Elevated;
    } else if (pressure_ == FramePressure::Elevated &&
               available <= config_.elevated_enter_available_q16 +
                                config_.recovery_hysteresis_q16) {
        next = FramePressure::Elevated;
    } else {
        next = FramePressure::Normal;
    }

    if (config_.linux_psi.enabled && snapshot.psi_available) {
        stats_.psi_samples = saturating_increment(stats_.psi_samples);
        const FramePressure floor =
            linux_psi_pressure_floor(snapshot, config_.linux_psi);
        if (pressure_rank(floor) > pressure_rank(next)) {
            stats_.psi_pressure_escalations =
                saturating_increment(stats_.psi_pressure_escalations);
        }
        next = higher_pressure(next, floor);
    }

    if (config_.windows_low_memory_elevated_floor &&
        snapshot.windows_low_memory_notification_available) {
        stats_.windows_low_memory_samples =
            saturating_increment(stats_.windows_low_memory_samples);
        if (snapshot.windows_low_memory_signaled && next == FramePressure::Normal) {
            next = FramePressure::Elevated;
            stats_.windows_low_memory_escalations =
                saturating_increment(stats_.windows_low_memory_escalations);
        }
    }

    if (next != pressure_) {
        pressure_ = next;
        stats_.pressure_changes = saturating_increment(stats_.pressure_changes);
    }
    stats_.pressure = pressure_;
    *pressure = pressure_;
    return true;
}

FramePressure ZenithProcessMemoryPressurePolicy::pressure() const noexcept {
    return pressure_;
}

ZenithProcessMemoryPressureStats ZenithProcessMemoryPressurePolicy::stats() const noexcept {
    return stats_;
}

void ZenithProcessMemoryPressurePolicy::reset() noexcept {
    pressure_ = FramePressure::Normal;
    stats_ = {};
}

bool capture_zenith_process_memory_snapshot(
    ZenithProcessMemorySnapshot* snapshot,
    std::string* error) {
    if (snapshot == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    ZenithProcessMemorySnapshot result;
#if defined(_WIN32)
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) {
        *error = "GlobalMemoryStatusEx failed";
        return false;
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            static_cast<DWORD>(sizeof(counters)))) {
        *error = "GetProcessMemoryInfo failed";
        return false;
    }
    result.process_rss_bytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
    result.system_available_bytes = memory.ullAvailPhys;
    result.system_total_bytes = memory.ullTotalPhys;

    ZenithWindowsMemoryContext context;
    std::string context_error;
    if (!capture_zenith_windows_memory_context(&context, &context_error)) {
        *error = context_error.empty()
                     ? "unable to capture Windows memory context"
                     : std::move(context_error);
        return false;
    }
    if (!apply_zenith_windows_memory_context(context, &result, &context_error)) {
        *error = context_error.empty()
                     ? "unable to apply Windows memory context"
                     : std::move(context_error);
        return false;
    }
#elif defined(__linux__)
    std::uint64_t total_kib = 0U;
    std::uint64_t available_kib = 0U;
    if (!linux_meminfo_value_kib("MemTotal", &total_kib) ||
        !linux_meminfo_value_kib("MemAvailable", &available_kib)) {
        *error = "unable to read Linux MemTotal/MemAvailable";
        return false;
    }
    std::ifstream statm("/proc/self/statm");
    std::uint64_t virtual_pages = 0U;
    std::uint64_t resident_pages = 0U;
    if (!(statm >> virtual_pages >> resident_pages)) {
        *error = "unable to read Linux process RSS";
        return false;
    }
    static_cast<void>(virtual_pages);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        *error = "unable to resolve Linux page size";
        return false;
    }
    constexpr std::uint64_t kKiB = 1024U;
    const std::uint64_t page_bytes = static_cast<std::uint64_t>(page_size);
    if (total_kib > std::numeric_limits<std::uint64_t>::max() / kKiB ||
        available_kib > std::numeric_limits<std::uint64_t>::max() / kKiB ||
        resident_pages > std::numeric_limits<std::uint64_t>::max() / page_bytes) {
        *error = "Linux memory snapshot overflow";
        return false;
    }
    result.process_rss_bytes = resident_pages * page_bytes;
    result.system_available_bytes = available_kib * kKiB;
    result.system_total_bytes = total_kib * kKiB;

    ZenithLinuxMemoryContext context;
    std::string context_error;
    if (!capture_zenith_linux_memory_context(&context, &context_error)) {
        *error = context_error.empty()
                     ? "unable to capture Linux memory context"
                     : std::move(context_error);
        return false;
    }
    if (!apply_zenith_linux_memory_context(context, &result, &context_error)) {
        *error = context_error.empty()
                     ? "unable to apply Linux memory context"
                     : std::move(context_error);
        return false;
    }
#else
    *error = "process memory snapshot is unsupported on this platform";
    return false;
#endif
    if (!result.valid()) {
        *error = "captured process memory snapshot is invalid";
        return false;
    }
    *snapshot = result;
    return true;
}

bool sample_and_apply_zenith_process_memory_pressure(
    ZenithProcessMemoryPressurePolicy* policy,
    ZenithProcessTabController* controller,
    ZenithProcessMemorySnapshot* captured,
    std::string* error) {
    if (policy == nullptr || controller == nullptr || error == nullptr) {
        return false;
    }
    ZenithProcessMemorySnapshot snapshot;
    if (!capture_zenith_process_memory_snapshot(&snapshot, error)) {
        return false;
    }
    FramePressure pressure = FramePressure::Normal;
    if (!policy->update(snapshot, &pressure)) {
        *error = "unable to evaluate process memory pressure";
        return false;
    }
    if (captured != nullptr) {
        *captured = snapshot;
    }
    if (controller->global_pressure() == pressure) {
        error->clear();
        return true;
    }
    return controller->set_global_pressure(pressure, error);
}

} // namespace zevryon::massivedoc
