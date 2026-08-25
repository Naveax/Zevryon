#pragma once

#include <cstdint>
#include <string>

namespace zevryon::massivedoc {

struct ZenithProcessMemorySnapshot;

struct ZenithWindowsMemoryContext {
    bool low_memory_notification_available{false};
    bool low_memory_signaled{false};
    bool job_detected{false};
    std::uint32_t job_active_processes{0U};
    bool process_memory_limit_enabled{false};
    std::uint64_t process_memory_limit_bytes{0U};
    bool job_memory_limit_enabled{false};
    std::uint64_t job_memory_limit_bytes{0U};
    std::uint64_t peak_process_memory_used_bytes{0U};
    std::uint64_t peak_job_memory_used_bytes{0U};
};

bool capture_zenith_windows_memory_context(
    ZenithWindowsMemoryContext* context,
    std::string* error);

bool apply_zenith_windows_memory_context(
    const ZenithWindowsMemoryContext& context,
    ZenithProcessMemorySnapshot* snapshot,
    std::string* error);

} // namespace zevryon::massivedoc
