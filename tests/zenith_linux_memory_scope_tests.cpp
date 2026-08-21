#include "zenith_linux_memory_scope.hpp"

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
    std::string path;
    require(
        parse_linux_cgroup_v2_path("0::/user.slice/test.scope\n", &path) &&
            path == "/user.slice/test.scope",
        "nested cgroup v2 path was not parsed");
    require(
        parse_linux_cgroup_v2_path("0::/\n", &path) && path == "/",
        "root cgroup v2 path was not parsed");
    require(
        !parse_linux_cgroup_v2_path("0::/safe/../escape\n", &path),
        "cgroup traversal path was accepted");
    require(
        !parse_linux_cgroup_v2_path("5:memory:/legacy\n", &path),
        "legacy cgroup line was accepted as v2");

    ZenithLinuxMemoryScopeObservation scope;
    require(
        parse_linux_cgroup_memory_values(
            "943718400\n", "1073741824\n", &scope),
        "finite cgroup memory values were not parsed");
    require(scope.cgroup_v2_detected, "cgroup v2 detection missing");
    require(scope.cgroup_v2_limited, "finite cgroup limit missing");
    require(scope.cgroup_current_bytes == 943718400U, "cgroup current mismatch");
    require(scope.cgroup_limit_bytes == 1073741824U, "cgroup limit mismatch");

    std::uint64_t effective_total = 0U;
    std::uint64_t effective_available = 0U;
    require(
        apply_linux_cgroup_memory_scope(
            16ULL * 1024ULL * 1024ULL * 1024ULL,
            8ULL * 1024ULL * 1024ULL * 1024ULL,
            scope,
            &effective_total,
            &effective_available),
        "finite cgroup scope application failed");
    require(
        effective_total == 1073741824U,
        "effective total did not clamp to cgroup limit");
    require(
        effective_available == 130023424U,
        "effective available did not use cgroup headroom");

    ZenithLinuxMemoryScopeObservation unlimited;
    require(
        parse_linux_cgroup_memory_values("4096\n", "max\n", &unlimited),
        "unlimited cgroup memory values were not parsed");
    require(
        unlimited.cgroup_v2_detected && !unlimited.cgroup_v2_limited,
        "unlimited cgroup state mismatch");
    require(
        apply_linux_cgroup_memory_scope(
            1000U, 700U, unlimited, &effective_total, &effective_available) &&
            effective_total == 1000U && effective_available == 700U,
        "unlimited cgroup changed host memory scope");

    ZenithLinuxMemoryScopeObservation psi;
    require(
        parse_linux_memory_psi(
            "some avg10=12.50 avg60=4.00 avg300=1.00 total=10\n"
            "full avg10=2.50 avg60=1.00 avg300=0.50 total=5\n",
            &psi),
        "PSI memory values were not parsed");
    require(psi.psi_available, "PSI availability missing");
    require(psi.psi_some_avg10_q16 == 8192U, "PSI some avg10 mismatch");
    require(psi.psi_full_avg10_q16 == 1638U, "PSI full avg10 mismatch");

    ZenithLinuxMemoryScopeObservation partial_psi;
    require(
        parse_linux_memory_psi(
            "some avg10=1.00 avg60=0.50 avg300=0.10 total=1\n",
            &partial_psi),
        "partial PSI some line was rejected");
    require(
        partial_psi.psi_available &&
            partial_psi.psi_some_avg10_q16 == 655U &&
            partial_psi.psi_full_avg10_q16 == 0U,
        "partial PSI values mismatch");

    ZenithLinuxMemoryScopeObservation bad;
    require(
        !parse_linux_cgroup_memory_values("oops", "1024", &bad),
        "invalid cgroup current value accepted");
    require(
        !parse_linux_memory_psi("some avg10=101.0 total=1\n", &bad),
        "out-of-range PSI percentage accepted");

    std::cout << "Zevryon Linux memory-scope tests passed\n";
    return 0;
}
