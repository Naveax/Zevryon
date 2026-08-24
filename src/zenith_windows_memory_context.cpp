#include "zenith_windows_memory_context.hpp"

#include "zenith_process_memory_pressure.hpp"

#include <limits>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace zevryon::massivedoc {

bool capture_zenith_windows_memory_context(
    ZenithWindowsMemoryContext* context,
    std::string* error) {
    if (context == nullptr || error == nullptr) {
        return false;
    }
    *context = {};
    error->clear();
#if defined(_WIN32)
    HANDLE notification =
        CreateMemoryResourceNotification(LowMemoryResourceNotification);
    if (notification != nullptr) {
        BOOL low_memory = FALSE;
        if (QueryMemoryResourceNotification(notification, &low_memory)) {
            context->low_memory_notification_available = true;
            context->low_memory_signaled = low_memory != FALSE;
        }
        CloseHandle(notification);
    }

    BOOL in_job = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &in_job)) {
        return true;
    }
    if (in_job == FALSE) {
        return true;
    }
    context->job_detected = true;

    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (QueryInformationJobObject(
            nullptr,
            JobObjectBasicAccountingInformation,
            &accounting,
            static_cast<DWORD>(sizeof(accounting)),
            nullptr)) {
        context->job_active_processes =
            static_cast<std::uint32_t>(accounting.ActiveProcesses);
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!QueryInformationJobObject(
            nullptr,
            JobObjectExtendedLimitInformation,
            &limits,
            static_cast<DWORD>(sizeof(limits)),
            nullptr)) {
        return true;
    }

    const DWORD flags = limits.BasicLimitInformation.LimitFlags;
    if ((flags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) != 0U) {
        context->process_memory_limit_enabled = true;
        context->process_memory_limit_bytes =
            static_cast<std::uint64_t>(limits.ProcessMemoryLimit);
    }
    if ((flags & JOB_OBJECT_LIMIT_JOB_MEMORY) != 0U) {
        context->job_memory_limit_enabled = true;
        context->job_memory_limit_bytes =
            static_cast<std::uint64_t>(limits.JobMemoryLimit);
    }
    context->peak_process_memory_used_bytes =
        static_cast<std::uint64_t>(limits.PeakProcessMemoryUsed);
    context->peak_job_memory_used_bytes =
        static_cast<std::uint64_t>(limits.PeakJobMemoryUsed);
#endif
    return true;
}

bool apply_zenith_windows_memory_context(
    const ZenithWindowsMemoryContext& context,
    ZenithProcessMemorySnapshot* snapshot,
    std::string* error) {
    if (snapshot == nullptr || error == nullptr || !snapshot->valid()) {
        if (error != nullptr) {
            *error = "invalid Windows memory context application";
        }
        return false;
    }
    error->clear();
    if (context.process_memory_limit_enabled &&
        context.process_memory_limit_bytes == 0U) {
        *error = "Windows process memory limit is zero";
        return false;
    }
    if (context.job_memory_limit_enabled && context.job_memory_limit_bytes == 0U) {
        *error = "Windows job memory limit is zero";
        return false;
    }

    snapshot->windows_low_memory_notification_available =
        context.low_memory_notification_available;
    snapshot->windows_low_memory_signaled =
        context.low_memory_notification_available && context.low_memory_signaled;
    snapshot->windows_job_detected = context.job_detected;
    snapshot->windows_job_active_processes =
        context.job_detected ? context.job_active_processes : 0U;
    snapshot->windows_process_memory_limit_enabled =
        context.job_detected && context.process_memory_limit_enabled;
    snapshot->windows_process_memory_limit_bytes =
        snapshot->windows_process_memory_limit_enabled
            ? context.process_memory_limit_bytes
            : 0U;
    snapshot->windows_job_memory_limit_enabled =
        context.job_detected && context.job_memory_limit_enabled;
    snapshot->windows_job_memory_limit_bytes =
        snapshot->windows_job_memory_limit_enabled
            ? context.job_memory_limit_bytes
            : 0U;
    snapshot->windows_peak_process_memory_used_bytes =
        context.job_detected ? context.peak_process_memory_used_bytes : 0U;
    snapshot->windows_peak_job_memory_used_bytes =
        context.job_detected ? context.peak_job_memory_used_bytes : 0U;
    return snapshot->valid();
}

} // namespace zevryon::massivedoc
