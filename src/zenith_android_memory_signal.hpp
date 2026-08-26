#pragma once

#include "device_frame_profile.hpp"
#include "frame_budget_scheduler.hpp"

#include <cstdint>
#include <string>

namespace zevryon::massivedoc {

// A complete current Android memory/lifecycle snapshot, not an event delta.
// Callers must carry forward still-active platform facts on every apply call;
// zero/false values mean the caller is authoritatively clearing that fact.
struct ZenithAndroidMemorySignal {
    // Current trim/lifecycle level to apply. Use zero only when the platform
    // shell has an authoritative reason to clear previously applied trim state.
    std::int32_t trim_level{0};
    // Current ActivityManager.isLowRamDevice(). This selects the conservative
    // profile in ZenithAndroidMemoryDecision; the bridge does not hot-swap the
    // device profile of already constructed tabs.
    bool low_ram_device{false};
    // Current ActivityManager.MemoryInfo.lowMemory. This is an explicit system
    // pressure signal and maps directly to Critical.
    bool system_low_memory{false};
};

struct ZenithAndroidMemoryDecision {
    // Profile decision for the Android shell/tab-construction lifecycle. The
    // pressure bridge applies only `pressure` to the process controller.
    DeviceFrameProfile profile{DeviceFrameProfile::MidPhone};
    FramePressure pressure{FramePressure::Normal};
    bool ui_hidden{false};
    bool background_lru{false};
};

bool evaluate_android_memory_signal(
    std::uint64_t total_ram_mib,
    const ZenithAndroidMemorySignal& signal,
    ZenithAndroidMemoryDecision* decision) noexcept;

class ZenithProcessTabController;

// Updates only the platform-memory pressure source. Process-memory pressure is
// maintained independently by the existing sampler/controller path, so callback
// ordering cannot clear a stronger source that remains active. The controller is
// owned by the process runtime/event-loop model; platform/JNI glue must marshal
// this call onto that same owner context instead of invoking it concurrently from
// an arbitrary callback thread. The returned decision may also carry a profile
// recommendation; this function does not apply that profile to existing tabs.
bool apply_android_memory_pressure_signal(
    std::uint64_t total_ram_mib,
    const ZenithAndroidMemorySignal& signal,
    ZenithProcessTabController* controller,
    ZenithAndroidMemoryDecision* decision,
    std::string* error);

} // namespace zevryon::massivedoc
