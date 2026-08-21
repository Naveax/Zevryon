#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace zevryon::massivedoc {

struct ZenithLinuxMemoryScopeObservation {
    bool cgroup_v2_detected{false};
    bool cgroup_v2_limited{false};
    std::uint64_t cgroup_current_bytes{0U};
    std::uint64_t cgroup_limit_bytes{0U};
    bool psi_available{false};
    std::uint32_t psi_some_avg10_q16{0U};
    std::uint32_t psi_full_avg10_q16{0U};
};

bool parse_linux_cgroup_v2_path(
    std::string_view cgroup_text,
    std::string* relative_path);

bool parse_linux_cgroup_memory_values(
    std::string_view current_text,
    std::string_view max_text,
    ZenithLinuxMemoryScopeObservation* observation) noexcept;

bool parse_linux_memory_psi(
    std::string_view pressure_text,
    ZenithLinuxMemoryScopeObservation* observation) noexcept;

bool apply_linux_cgroup_memory_scope(
    std::uint64_t host_total_bytes,
    std::uint64_t host_available_bytes,
    const ZenithLinuxMemoryScopeObservation& observation,
    std::uint64_t* effective_total_bytes,
    std::uint64_t* effective_available_bytes) noexcept;

} // namespace zevryon::massivedoc
