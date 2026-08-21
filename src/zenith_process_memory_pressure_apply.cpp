#include "zenith_process_memory_pressure.hpp"

#include "zenith_process_tab_controller.hpp"

namespace zevryon::massivedoc {

bool apply_zenith_process_memory_pressure_snapshot(
    ZenithProcessMemoryPressurePolicy* policy,
    ZenithProcessTabController* controller,
    const ZenithProcessMemorySnapshot& snapshot,
    std::string* error) {
    if (policy == nullptr || controller == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    FramePressure pressure = FramePressure::Normal;
    if (!policy->update(snapshot, &pressure)) {
        *error = "unable to evaluate process memory pressure snapshot";
        return false;
    }
    if (controller->global_pressure() == pressure) {
        return true;
    }
    return controller->set_global_pressure(pressure, error);
}

} // namespace zevryon::massivedoc
