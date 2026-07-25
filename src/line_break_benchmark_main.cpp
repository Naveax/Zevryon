#include "line_break_opportunity.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <numeric>
#include <vector>

namespace {

class TrackingLimitResource final : public std::pmr::memory_resource {
public:
    explicit TrackingLimitResource(std::size_t hard_limit) noexcept
        : hard_limit_(hard_limit) {}

    std::size_t current() const noexcept { return current_; }
    std::size_t peak() const noexcept { return peak_; }
    std::size_t rejected() const noexcept { return rejected_; }
    bool accounting_clean() const noexcept { return accounting_clean_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > hard_limit_ - current_) {
            ++rejected_;
            throw std::bad_alloc();
        }
        void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        current_ += bytes;
        peak_ = std::max(peak_, current_);
        return result;
    }

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        if (bytes > current_) {
            accounting_clean_ = false;
            current_ = 0U;
        } else {
            current_ -= bytes;
        }
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t hard_limit_;
    std::size_t current_{0U};
    std::size_t peak_{0U};
    std::size_t rejected_{0U};
    bool accounting_clean_{true};
};

std::uint8_t utf8_length(std::uint32_t value) noexcept {
    if (value <= 0x7FU) {
        return 1U;
    }
    if (value <= 0x7FFU) {
        return 2U;
    }
    if (value <= 0xFFFFU) {
        return 3U;
    }
    return 4U;
}

struct Fixture final {
    std::vector<zevryon::text::DecodedCodePoint> codepoints;
    std::vector<zevryon::text::GraphemeBoundary> boundaries;
    std::uint64_t source_bytes{0U};
};

void append_cluster(Fixture* fixture, std::initializer_list<std::uint32_t> values) {
    fixture->boundaries.push_back({
        fixture->source_bytes,
        static_cast<std::uint32_t>(fixture->codepoints.size())});
    for (std::uint32_t value : values) {
        const std::uint8_t length = utf8_length(value);
        fixture->codepoints.push_back({
            value,
            fixture->source_bytes,
            fixture->source_bytes + length,
            false});
        fixture->source_bytes += length;
    }
}

Fixture make_fixture() {
    Fixture fixture;
    fixture.codepoints.reserve(24U * 1024U);
    fixture.boundaries.reserve(36U * 1024U + 1U);
    for (std::size_t block = 0U; block < 1024U; ++block) {
        append_cluster(&fixture, {U'A'});
        append_cluster(&fixture, {U'B'});
        append_cluster(&fixture, {U' '});
        append_cluster(&fixture, {U'C'});
        append_cluster(&fixture, {U'D'});
        append_cluster(&fixture, {U'-'});
        append_cluster(&fixture, {U'E'});
        append_cluster(&fixture, {U'F'});
        append_cluster(&fixture, {0x200BU});
        append_cluster(&fixture, {U'G'});
        append_cluster(&fixture, {0x2060U});
        append_cluster(&fixture, {U'H'});
        append_cluster(&fixture, {0x4E00U});
        append_cluster(&fixture, {0x4E8CU});
        append_cluster(&fixture, {0x1F1E6U});
        append_cluster(&fixture, {0x1F1E7U});
        append_cluster(&fixture, {0x261DU});
        append_cluster(&fixture, {0x1F3FBU});
        append_cluster(&fixture, {0x1100U});
        append_cluster(&fixture, {0x1160U});
        append_cluster(&fixture, {0x0E01U, 0x0E31U});
        append_cluster(&fixture, {0x000AU});
        for (std::size_t index = 0U; index < 14U; ++index) {
            append_cluster(&fixture, {
                static_cast<std::uint32_t>(U'a' + (index % 26U))});
        }
    }
    fixture.boundaries.push_back({
        fixture.source_bytes,
        static_cast<std::uint32_t>(fixture.codepoints.size())});
    assert(fixture.source_bytes == 65536U);
    return fixture;
}

double percentile(std::vector<double> values, double probability) {
    std::sort(values.begin(), values.end());
    const double position = probability * static_cast<double>(values.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

std::uint64_t checksum(const zevryon::text::LineBreakOpportunityMap& map) noexcept {
    std::uint64_t value = 1469598103934665603ULL;
    for (std::uint8_t opportunity : map.opportunities) {
        value ^= opportunity;
        value *= 1099511628211ULL;
    }
    return value;
}

} // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 512U;
    if (argc >= 2) {
        try {
            iterations = static_cast<std::size_t>(std::stoull(argv[1]));
        } catch (...) {
            std::cerr << "invalid iteration count\n";
            return 2;
        }
    }
    if (iterations == 0U) {
        std::cerr << "iteration count must be positive\n";
        return 2;
    }

    const Fixture fixture = make_fixture();
    constexpr std::size_t kHardLimit = 524288U;
    TrackingLimitResource resource(kHardLimit);
    zevryon::text::LineBreakOpportunityMap map(&resource);
    zevryon::text::LineBreakOpportunityStats stats;
    zevryon::text::LineBreakOpportunityError error;
    const zevryon::text::LineBreakOpportunityRequest request{
        std::span<const zevryon::text::DecodedCodePoint>(fixture.codepoints),
        std::span<const zevryon::text::GraphemeBoundary>(fixture.boundaries)};

    for (std::size_t warmup = 0U; warmup < 16U; ++warmup) {
        if (!zevryon::text::build_line_break_opportunity_map(
                request,
                &map,
                &stats,
                &error)) {
            std::cerr << "warmup failed: " << error.message << '\n';
            return 1;
        }
    }

    std::vector<double> samples;
    samples.reserve(iterations);
    std::uint64_t digest = 0U;
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        const bool ok = zevryon::text::build_line_break_opportunity_map(
            request,
            &map,
            &stats,
            &error);
        const auto finish = std::chrono::steady_clock::now();
        if (!ok) {
            std::cerr << "benchmark failed: " << error.message << '\n';
            return 1;
        }
        samples.push_back(
            std::chrono::duration<double, std::milli>(finish - start).count());
        digest ^= checksum(map) + 0x9E3779B97F4A7C15ULL +
                  (digest << 6U) + (digest >> 2U);
    }

    const double p50 = percentile(samples, 0.50);
    const double p95 = percentile(samples, 0.95);
    const double p99 = percentile(samples, 0.99);
    const double maximum = *std::max_element(samples.begin(), samples.end());
    const bool within_hard_limits =
        resource.current() == map.opportunities.size() &&
        resource.peak() <= kHardLimit &&
        resource.rejected() == 0U;

    std::cout << "{\n"
              << "  \"schema\": \"zevryon.line-break-opportunity-benchmark.v1\",\n"
              << "  \"unicode_version\": \""
              << zevryon::text::kUnicodeLineBreakDataVersion << "\",\n"
              << "  \"fixture_bytes\": " << fixture.source_bytes << ",\n"
              << "  \"input_codepoints\": " << fixture.codepoints.size() << ",\n"
              << "  \"input_clusters\": " << stats.input_clusters << ",\n"
              << "  \"output_boundaries\": " << stats.output_boundaries << ",\n"
              << "  \"record_bytes\": 1,\n"
              << "  \"significant_clusters\": " << stats.significant_clusters << ",\n"
              << "  \"ignored_combining_clusters\": "
              << stats.ignored_combining_clusters << ",\n"
              << "  \"mandatory_boundaries\": " << stats.mandatory_boundaries << ",\n"
              << "  \"allowed_boundaries\": " << stats.allowed_boundaries << ",\n"
              << "  \"prohibited_boundaries\": " << stats.prohibited_boundaries << ",\n"
              << "  \"current_bytes\": " << resource.current() << ",\n"
              << "  \"peak_bytes\": " << resource.peak() << ",\n"
              << "  \"hard_limit_bytes\": " << kHardLimit << ",\n"
              << "  \"iterations\": " << iterations << ",\n"
              << "  \"warmup_iterations\": 16,\n"
              << "  \"p50_ms\": " << p50 << ",\n"
              << "  \"p95_ms\": " << p95 << ",\n"
              << "  \"p99_ms\": " << p99 << ",\n"
              << "  \"maximum_ms\": " << maximum << ",\n"
              << "  \"checksum\": " << checksum(map) << ",\n"
              << "  \"distribution_checksum\": " << digest << ",\n"
              << "  \"rejected_reservations\": " << resource.rejected() << ",\n"
              << "  \"accounting_clean\": "
              << (resource.accounting_clean() ? "true" : "false") << ",\n"
              << "  \"within_hard_limits\": "
              << (within_hard_limits ? "true" : "false") << "\n}\n";
    return resource.accounting_clean() && within_hard_limits ? 0 : 1;
}
