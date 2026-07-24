#include "font_content_identity.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

using namespace zevryon::text;

constexpr std::size_t kPayloadBytes = 16U * 1024U * 1024U;
constexpr std::size_t kWarmupIterations = 3U;
constexpr std::size_t kMeasuredIterations = 24U;

double percentile(const std::vector<double>& sorted, double fraction) {
    const double position =
        fraction * static_cast<double>(sorted.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1U, sorted.size() - 1U);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

} // namespace

int main() {
    std::vector<std::byte> payload(kPayloadBytes);
    std::uint32_t state = 0x9e3779b9U;
    for (std::size_t index = 0U; index < payload.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        payload[index] = std::byte{static_cast<unsigned char>(state)};
    }

    std::vector<double> durations;
    durations.reserve(kMeasuredIterations);
    FontContentIdentity expected;
    for (std::size_t iteration = 0U;
         iteration < kWarmupIterations + kMeasuredIterations;
         ++iteration) {
        FontContentIdentity identity;
        const auto begin = std::chrono::steady_clock::now();
        const bool ok = compute_font_content_identity(payload, 17U, &identity);
        const auto end = std::chrono::steady_clock::now();
        if (!ok || (iteration != 0U && identity != expected)) {
            std::cerr << "font identity benchmark produced inconsistent output\n";
            return 1;
        }
        expected = identity;
        if (iteration >= kWarmupIterations) {
            durations.push_back(
                std::chrono::duration<double, std::milli>(end - begin).count());
        }
    }

    std::sort(durations.begin(), durations.end());
    const double p50 = percentile(durations, 0.50);
    const double p95 = percentile(durations, 0.95);
    const double p99 = percentile(durations, 0.99);
    const double maximum = durations.back();
    const double throughput = p50 == 0.0
        ? 0.0
        : (static_cast<double>(kPayloadBytes) / (1024.0 * 1024.0)) /
              (p50 / 1000.0);

    std::cout << std::fixed << std::setprecision(6)
              << "{\"schema\":\"zevryon.font-content-identity-benchmark.v1\","
              << "\"payload_bytes\":" << kPayloadBytes << ','
              << "\"face_index\":17,"
              << "\"warmup_iterations\":" << kWarmupIterations << ','
              << "\"iterations\":" << kMeasuredIterations << ','
              << "\"identity_high\":" << expected.high << ','
              << "\"identity_low\":" << expected.low << ','
              << "\"p50_ms\":" << p50 << ','
              << "\"p95_ms\":" << p95 << ','
              << "\"p99_ms\":" << p99 << ','
              << "\"maximum_ms\":" << maximum << ','
              << "\"p50_throughput_mib_s\":" << throughput
              << "}\n";
    return 0;
}
