#include "frame_latency_sample_collector.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;
using namespace std::chrono_literals;

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
    require(FrameLatencySampleCollectorConfig{2U, 3U}.valid(), "valid config rejected");
    require(!FrameLatencySampleCollectorConfig{0U, 0U}.valid(), "zero retention accepted");

    FrameLatencySampleCollector collector({2U, 3U});
    require(collector.valid(), "collector invalid");
    require(!collector.observe(10us), "warmup one recorded");
    require(!collector.observe(11us), "warmup two recorded");
    require(collector.observe(12us), "sample one rejected");
    require(collector.observe(13us), "sample two rejected");
    require(collector.observe(14us), "sample three rejected");
    require(!collector.observe(15us), "capacity overflow retained");

    const auto status = collector.status();
    require(status.observations == 6U, "observation count mismatch");
    require(status.warmup_discarded == 2U, "warmup count mismatch");
    require(status.recorded == 3U, "recorded count mismatch");
    require(status.capacity_drops == 1U, "capacity drop mismatch");
    require(collector.samples_ns().size() == 3U, "sample vector mismatch");
    require(collector.samples_ns()[0] == 12'000U, "sample conversion mismatch");

    collector.reset();
    require(collector.samples_ns().empty(), "reset retained samples");
    require(collector.status().observations == 0U, "reset retained counters");

    std::cout << "Zevryon frame-latency sample collector tests passed\n";
    return 0;
}
