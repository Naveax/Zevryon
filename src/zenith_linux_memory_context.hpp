#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace zevryon::massivedoc {

struct ZenithProcessMemorySnapshot;

struct ZenithLinuxMemoryContext {
    bool cgroup_v2_detected{false};
    bool cgroup_v2_limited{false};
    std::uint64_t cgroup_limit_bytes{0U};
    std::uint64_t cgroup_current_bytes{0U};
    bool psi_available{false};
    std::uint32_t psi_some_avg10_milli_percent{0U};
    std::uint32_t psi_full_avg10_milli_percent{0U};
};

bool parse_zenith_cgroup_v2_memory(
    std::string_view memory_max,
    std::string_view memory_current,
    ZenithLinuxMemoryContext* context,
    std::string* error);

bool parse_zenith_linux_memory_psi(
    std::string_view pressure_text,
    ZenithLinuxMemoryContext* context,
    std::string* error);

bool capture_zenith_linux_memory_context(
    ZenithLinuxMemoryContext* context,
    std::string* error);

bool apply_zenith_linux_memory_context(
    const ZenithLinuxMemoryContext& context,
    ZenithProcessMemorySnapshot* snapshot,
    std::string* error);

} // namespace zevryon::massivedoc
