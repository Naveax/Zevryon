#include "zenith_windows_memory_scope.hpp"

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
    if (!value) fail(message);
}
} // namespace

int main() {
    std::uint64_t total = 0U;
    std::uint64_t available = 0U;

    ZenithWindowsMemoryScopeObservation unlimited;
    require(
        apply_windows_process_memory_scope(
            1000U, 700U, unlimited, &total, &available) &&
            total == 1000U && available == 700U,
        "unlimited Windows scope changed host memory");

    ZenithWindowsMemoryScopeObservation limited;
    limited.process_memory_limited = true;
    limited.private_commit_bytes = 400U;
    limited.process_memory_limit_bytes = 512U;
    require(
        apply_windows_process_memory_scope(
            2000U, 1500U, limited, &total, &available) &&
            total == 512U && available == 112U,
        "Windows process memory limit was not applied");

    limited.private_commit_bytes = 600U;
    require(
        apply_windows_process_memory_scope(
            2000U, 1500U, limited, &total, &available) &&
            total == 512U && available == 0U,
        "Windows exhausted process limit did not clamp available bytes to zero");

    limited.process_memory_limit_bytes = 0U;
    require(
        !apply_windows_process_memory_scope(
            2000U, 1500U, limited, &total, &available),
        "invalid zero Windows process limit was accepted");

    require(
        !apply_windows_process_memory_scope(
            1000U, 1001U, unlimited, &total, &available),
        "invalid host memory scope was accepted");

    std::cout << "Zevryon Windows memory-scope tests passed\n";
    return 0;
}
