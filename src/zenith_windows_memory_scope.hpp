#pragma once

#include <cstdint>

namespace zevryon::massivedoc {

struct ZenithWindowsMemoryScopeObservation {
    bool process_memory_limited{false};
    std::uint64_t private_commit_bytes{0U};
    std::uint64_t process_memory_limit_bytes{0U};
};

bool apply_windows_process_memory_scope(
    std::uint64_t host_total_bytes,
    std::uint64_t host_available_bytes,
    const ZenithWindowsMemoryScopeObservation& observation,
    std::uint64_t* effective_total_bytes,
    std::uint64_t* effective_available_bytes) noexcept;

} // namespace zevryon::massivedoc
