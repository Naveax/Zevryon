#include "zenith_process_memory_pressure.hpp"
#include "zenith_windows_memory_context.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        fail(message);
    }
}

} // namespace

int main() {
    std::string error;
    ZenithProcessMemorySnapshot snapshot{123U, 600U, 1000U};
    ZenithWindowsMemoryContext context;
    context.low_memory_notification_available = true;
    context.low_memory_signaled = true;
    context.job_detected = true;
    context.job_active_processes = 3U;
    context.process_memory_limit_enabled = true;
    context.process_memory_limit_bytes = 512U;
    context.job_memory_limit_enabled = true;
    context.job_memory_limit_bytes = 2048U;
    context.peak_process_memory_used_bytes = 256U;
    context.peak_job_memory_used_bytes = 1024U;

    require(
        apply_zenith_windows_memory_context(context, &snapshot, &error),
        "valid Windows memory context was rejected");
    require(snapshot.valid(), "Windows context produced invalid snapshot");
    require(
        snapshot.windows_low_memory_notification_available,
        "low-memory notification availability was lost");
    require(snapshot.windows_low_memory_signaled, "low-memory signal was lost");
    require(snapshot.windows_job_detected, "job detection was lost");
    require(
        snapshot.windows_job_active_processes == 3U,
        "job active-process count changed");
    require(
        snapshot.windows_process_memory_limit_enabled &&
            snapshot.windows_process_memory_limit_bytes == 512U,
        "process memory limit metadata changed");
    require(
        snapshot.windows_job_memory_limit_enabled &&
            snapshot.windows_job_memory_limit_bytes == 2048U,
        "job memory limit metadata changed");
    require(
        snapshot.windows_peak_process_memory_used_bytes == 256U &&
            snapshot.windows_peak_job_memory_used_bytes == 1024U,
        "job peak-memory accounting changed");

    ZenithWindowsMemoryContext unavailable;
    unavailable.low_memory_signaled = true;
    ZenithProcessMemorySnapshot unavailable_snapshot{123U, 600U, 1000U};
    require(
        apply_zenith_windows_memory_context(
            unavailable,
            &unavailable_snapshot,
            &error),
        "unavailable low-memory context was rejected");
    require(
        !unavailable_snapshot.windows_low_memory_signaled,
        "signal without notification authority was retained");

    ZenithWindowsMemoryContext invalid_limit;
    invalid_limit.job_detected = true;
    invalid_limit.process_memory_limit_enabled = true;
    ZenithProcessMemorySnapshot invalid_snapshot{123U, 600U, 1000U};
    require(
        !apply_zenith_windows_memory_context(
            invalid_limit,
            &invalid_snapshot,
            &error),
        "zero Windows process memory limit was accepted");

    ZenithWindowsMemoryContext captured;
    require(
        capture_zenith_windows_memory_context(&captured, &error),
        "platform Windows context capture fallback failed");

    std::cout << "Zevryon Windows memory-context tests passed\n";
    return 0;
}
