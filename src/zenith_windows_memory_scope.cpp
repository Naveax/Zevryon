#include "zenith_windows_memory_scope.hpp"

#include <algorithm>

namespace zevryon::massivedoc {

bool apply_windows_process_memory_scope(
    std::uint64_t host_total_bytes,
    std::uint64_t host_available_bytes,
    const ZenithWindowsMemoryScopeObservation& observation,
    std::uint64_t* effective_total_bytes,
    std::uint64_t* effective_available_bytes) noexcept {
    if (effective_total_bytes == nullptr || effective_available_bytes == nullptr ||
        host_total_bytes == 0U || host_available_bytes > host_total_bytes) {
        return false;
    }

    std::uint64_t total = host_total_bytes;
    std::uint64_t available = host_available_bytes;
    if (observation.process_memory_limited) {
        if (observation.process_memory_limit_bytes == 0U) {
            return false;
        }
        total = std::min(host_total_bytes, observation.process_memory_limit_bytes);
        const std::uint64_t process_available =
            observation.private_commit_bytes >= observation.process_memory_limit_bytes
                ? 0U
                : observation.process_memory_limit_bytes -
                      observation.private_commit_bytes;
        available = std::min({host_available_bytes, process_available, total});
    }

    *effective_total_bytes = total;
    *effective_available_bytes = available;
    return true;
}

} // namespace zevryon::massivedoc
