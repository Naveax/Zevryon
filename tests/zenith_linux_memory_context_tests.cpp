#include "zenith_linux_memory_context.hpp"
#include "zenith_process_memory_pressure.hpp"

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

    ZenithLinuxMemoryContext finite;
    require(
        parse_zenith_cgroup_v2_memory("512\n", "400\n", &finite, &error),
        "finite cgroup v2 controls were rejected");
    require(finite.cgroup_v2_detected, "finite cgroup was not detected");
    require(finite.cgroup_v2_limited, "finite cgroup limit was not admitted");
    require(finite.cgroup_limit_bytes == 512U, "finite cgroup limit changed");
    require(finite.cgroup_current_bytes == 400U, "cgroup current changed");

    ZenithLinuxMemoryContext unlimited;
    require(
        parse_zenith_cgroup_v2_memory("max\n", "99\n", &unlimited, &error),
        "unlimited cgroup v2 controls were rejected");
    require(unlimited.cgroup_v2_detected, "unlimited cgroup was not detected");
    require(!unlimited.cgroup_v2_limited, "unlimited cgroup became limited");
    require(unlimited.cgroup_limit_bytes == 0U, "unlimited cgroup gained a limit");

    ZenithLinuxMemoryContext psi;
    require(
        parse_zenith_linux_memory_psi(
            "some avg10=12.34 avg60=3.00 avg300=1.00 total=7\n"
            "full avg10=0.56 avg60=0.20 avg300=0.10 total=2\n",
            &psi,
            &error),
        "valid Linux memory PSI was rejected");
    require(psi.psi_available, "PSI availability was not recorded");
    require(
        psi.psi_some_avg10_milli_percent == 12'340U,
        "PSI some avg10 fixed-point conversion changed");
    require(
        psi.psi_full_avg10_milli_percent == 560U,
        "PSI full avg10 fixed-point conversion changed");

    ZenithLinuxMemoryContext invalid_psi;
    require(
        !parse_zenith_linux_memory_psi(
            "some avg10=100.001 avg60=0.00 avg300=0.00 total=1\n",
            &invalid_psi,
            &error),
        "out-of-range PSI was accepted");

    ZenithProcessMemorySnapshot effective{123U, 600U, 1000U};
    finite.psi_available = true;
    finite.psi_some_avg10_milli_percent = 12'340U;
    finite.psi_full_avg10_milli_percent = 560U;
    require(
        apply_zenith_linux_memory_context(finite, &effective, &error),
        "finite cgroup context failed to apply");
    require(effective.valid(), "effective cgroup snapshot is invalid");
    require(
        effective.memory_domain == ZenithProcessMemoryDomain::CgroupV2,
        "effective memory domain did not switch to cgroup v2");
    require(effective.cgroup_v2_detected, "snapshot lost cgroup detection");
    require(effective.cgroup_v2_limited, "snapshot lost cgroup limit authority");
    require(effective.system_total_bytes == 512U, "effective total changed");
    require(effective.system_available_bytes == 112U, "effective available changed");
    require(effective.psi_available, "snapshot lost PSI metadata");

    ZenithLinuxMemoryContext wider;
    require(
        parse_zenith_cgroup_v2_memory("2000", "100", &wider, &error),
        "wider cgroup controls were rejected");
    ZenithProcessMemorySnapshot host{123U, 600U, 1000U};
    require(
        apply_zenith_linux_memory_context(wider, &host, &error),
        "wider cgroup context failed to apply");
    require(
        host.memory_domain == ZenithProcessMemoryDomain::Host,
        "non-constraining cgroup replaced host domain");
    require(!host.cgroup_v2_limited, "non-constraining cgroup became effective");
    require(host.system_total_bytes == 1000U, "host total changed unexpectedly");
    require(host.system_available_bytes == 600U, "host available changed unexpectedly");

    ZenithLinuxMemoryContext exhausted;
    require(
        parse_zenith_cgroup_v2_memory("512", "700", &exhausted, &error),
        "over-limit cgroup sample was rejected");
    ZenithProcessMemorySnapshot exhausted_snapshot{123U, 600U, 1000U};
    require(
        apply_zenith_linux_memory_context(exhausted, &exhausted_snapshot, &error),
        "over-limit cgroup context failed to apply");
    require(
        exhausted_snapshot.system_available_bytes == 0U,
        "over-limit cgroup did not clamp available memory to zero");

    std::cout << "Zevryon Linux memory-context tests passed\n";
    return 0;
}
