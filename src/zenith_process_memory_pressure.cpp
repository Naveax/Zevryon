#include "zenith_process_memory_pressure.hpp"

#include "zenith_linux_memory_scope.hpp"
#include "zenith_process_tab_controller.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

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

bool read_linux_small_file(
    const std::filesystem::path& path,
    std::string* text) {
    if (text == nullptr) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if ((!input.good() && !input.eof()) || buffer.tellp() < 0 ||
        static_cast<std::uint64_t>(buffer.tellp()) > 64U * 1024U) {
        return false;
    }
    *text = buffer.str();
    return true;
}

void capture_linux_scope_signals(
    std::uint64_t host_total_bytes,
    std::uint64_t host_available_bytes,
    ZenithProcessMemorySnapshot* result) {
    if (result == nullptr) {
        return;
    }

    ZenithLinuxMemoryScopeObservation scope;
    std::string cgroup_text;
    std::string cgroup_path;
    std::filesystem::path cgroup_directory;

    if (read_linux_small_file("/proc/self/cgroup", &cgroup_text) &&
        parse_linux_cgroup_v2_path(cgroup_text, &cgroup_path)) {
        cgroup_directory = "/sys/fs/cgroup";
        if (cgroup_path.size() > 1U) {
            cgroup_directory /= cgroup_path.substr(1U);
        }

        std::string current_text;
        std::string max_text;
        if (read_linux_small_file(cgroup_directory / "memory.current", &current_text) &&
            read_linux_small_file(cgroup_directory / "memory.max", &max_text) &&
            parse_linux_cgroup_memory_values(current_text, max_text, &scope)) {
            std::uint64_t effective_total = host_total_bytes;
            std::uint64_t effective_available = host_available_bytes;
            if (apply_linux_cgroup_memory_scope(
                    host_total_bytes,
                    host_available_bytes,
                    scope,
                    &effective_total,
                    &effective_available)) {
                result->system_total_bytes = effective_total;
                result->system_available_bytes = effective_available;
                result->cgroup_v2_limited = scope.cgroup_v2_limited;
            }
        }

        std::string pressure_text;
        if (read_linux_small_file(
                cgroup_directory / "memory.pressure",
                &pressure_text) &&
            parse_linux_memory_psi(pressure_text, &scope)) {
            result->psi_memory_available = scope.psi_available;
            result->psi_some_avg10_q16 = scope.psi_some_avg10_q16;
            result->psi_full_avg10_q16 = scope.psi_full_avg10_q16;
            return;
        }
    }

    std::string pressure_text;
    if (read_linux_small_file("/proc/pressure/memory", &pressure_text) &&
        parse_linux_memory_psi(pressure_text, &scope)) {
        result->psi_memory_available = scope.psi_available;
        result->psi_some_avg10_q16 = scope.psi_some_avg10_q16;
        result->psi_full_avg10_q16 = scope.psi_full_avg10_q16;
    }
}
#endif

} // namespace

bool ZenithProcessMemorySnapshot::valid() const noexcept {
    return system_total_bytes > 0U &&
           system_available_bytes <= system_total_bytes &&
           (!psi_memory_available ||
            (psi_some_avg10_q16 <= kQ16One &&
             psi_full_avg10_q16 <= kQ16One));
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
           psi_some_elevated_avg10_q16 > psi_recovery_hysteresis_q16 &&
           psi_some_elevated_avg10_q16 <= kQ16One &&
           psi_full_critical_avg10_q16 > psi_recovery_hysteresis_q16 &&
           psi_full_critical_avg10_q16 <= kQ16One &&
           psi_recovery_hysteresis_q16 > 0U;
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
    const bool psi_critical_enter =
        snapshot.psi_memory_available &&
        snapshot.psi_full_avg10_q16 >= config_.psi_full_critical_avg10_q16;
    const bool psi_critical_hold =
        snapshot.psi_memory_available &&
        pressure_ == FramePressure::Critical &&
        snapshot.psi_full_avg10_q16 >=
            config_.psi_full_critical_avg10_q16 -
                config_.psi_recovery_hysteresis_q16;
    const bool psi_elevated_enter =
        snapshot.psi_memory_available &&
        snapshot.psi_some_avg10_q16 >= config_.psi_some_elevated_avg10_q16;
    const bool psi_elevated_hold =
        snapshot.psi_memory_available &&
        pressure_ == FramePressure::Elevated &&
        snapshot.psi_some_avg10_q16 >=
            config_.psi_some_elevated_avg10_q16 -
                config_.psi_recovery_hysteresis_q16;

    FramePressure next = pressure_;
    if (psi_critical_enter ||
        available <= config_.critical_enter_available_q16) {
        next = FramePressure::Critical;
    } else if (
        psi_critical_hold ||
        (pressure_ == FramePressure::Critical &&
         available <= config_.critical_enter_available_q16 +
                          config_.recovery_hysteresis_q16)) {
        next = FramePressure::Critical;
    } else if (
        psi_elevated_enter ||
        available <= config_.elevated_enter_available_q16) {
        next = FramePressure::Elevated;
    } else if (
        psi_elevated_hold ||
        (pressure_ == FramePressure::Elevated &&
         available <= config_.elevated_enter_available_q16 +
                          config_.recovery_hysteresis_q16)) {
        next = FramePressure::Elevated;
    } else {
        next = FramePressure::Normal;
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
    capture_linux_scope_signals(
        result.system_total_bytes,
        result.system_available_bytes,
        &result);
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
