#include "resource_ledger.hpp"
#include "rust_resource_ledger.hpp"
#include "shadow_resource_ledger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;
using zevryon::core::ResourceSnapshot;
using zevryon::core::RustResourceLedger;
using zevryon::core::ShadowResourceLedger;
using zevryon::core::resource_class_count;

enum class OperationKind : std::uint8_t {
    Reserve = 0,
    Release,
    CacheHit,
    CacheMiss,
    Eviction,
    PhysicalRead,
    PhysicalWrite,
};

struct Operation {
    OperationKind kind{OperationKind::Reserve};
    ResourceClass resource_class{ResourceClass::SourceWindow};
    std::size_t bytes{0};
    std::uint64_t value{0};
};

struct Metrics {
    double p50{0.0};
    double p95{0.0};
    double p99{0.0};
    double maximum{0.0};
    double operations_per_second{0.0};
};

std::atomic<std::uint64_t> benchmark_sink{0U};

std::uint64_t next_random(std::uint64_t& state) noexcept {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

std::uint64_t mix(std::uint64_t checksum, std::uint64_t value) noexcept {
    checksum ^= value + 0x9E3779B97F4A7C15ULL + (checksum << 6U) + (checksum >> 2U);
    return checksum;
}

std::vector<Operation> generate_workload(std::size_t count) {
    std::vector<Operation> operations;
    operations.reserve(count);
    std::uint64_t state = 0xBB67AE8584CAA73BULL;

    for (std::size_t index = 0U; index < count; ++index) {
        const std::uint64_t random = next_random(state);
        Operation operation{};
        operation.resource_class = static_cast<ResourceClass>(
            static_cast<std::size_t>(
                random % static_cast<std::uint64_t>(resource_class_count)));
        operation.bytes =
            1U + static_cast<std::size_t>((random >> 12U) % 4'096U);
        operation.value = random ^ (static_cast<std::uint64_t>(index) << 17U);

        if (index % 97U == 0U) {
            operation.kind = OperationKind::Reserve;
            operation.bytes = 2U * 1'024U * 1'024U;
        } else {
            const std::uint64_t selector = (random >> 28U) % 100U;
            if (selector < 38U) {
                operation.kind = OperationKind::Reserve;
            } else if (selector < 62U) {
                operation.kind = OperationKind::Release;
            } else if (selector < 72U) {
                operation.kind = OperationKind::CacheHit;
            } else if (selector < 81U) {
                operation.kind = OperationKind::CacheMiss;
            } else if (selector < 87U) {
                operation.kind = OperationKind::Eviction;
            } else if (selector < 94U) {
                operation.kind = OperationKind::PhysicalRead;
            } else {
                operation.kind = OperationKind::PhysicalWrite;
            }
        }
        operations.push_back(operation);
    }
    return operations;
}

struct CppAdapter {
    ResourceLedger ledger{};

    bool configure() noexcept {
        for (std::size_t index = 0U; index < resource_class_count; ++index) {
            ledger.set_hard_limit(
                static_cast<ResourceClass>(index),
                256U * 1'024U + index * 4'096U);
        }
        return true;
    }

    bool reserve(ResourceClass resource_class, std::size_t bytes) noexcept {
        return ledger.try_reserve(resource_class, bytes);
    }
    bool release(ResourceClass resource_class, std::size_t bytes) noexcept {
        ledger.release(resource_class, bytes);
        return true;
    }
    bool cache_hit(ResourceClass resource_class) noexcept {
        ledger.record_cache_hit(resource_class);
        return true;
    }
    bool cache_miss(ResourceClass resource_class) noexcept {
        ledger.record_cache_miss(resource_class);
        return true;
    }
    bool eviction(ResourceClass resource_class) noexcept {
        ledger.record_eviction(resource_class);
        return true;
    }
    bool physical_read(ResourceClass resource_class, std::uint64_t bytes) noexcept {
        ledger.record_physical_read(resource_class, bytes);
        return true;
    }
    bool physical_write(ResourceClass resource_class, std::uint64_t bytes) noexcept {
        ledger.record_physical_write(resource_class, bytes);
        return true;
    }
    bool snapshot(ResourceClass resource_class, ResourceSnapshot& output) const noexcept {
        output = ledger.snapshot(resource_class);
        return true;
    }
    std::size_t total_current_bytes() const noexcept {
        return ledger.total_current_bytes();
    }
    std::size_t total_peak_bytes() const noexcept {
        return ledger.total_peak_bytes();
    }
    bool within_hard_limits() const noexcept {
        return ledger.within_hard_limits();
    }
    bool accounting_clean() const noexcept {
        return ledger.accounting_clean();
    }
    bool verify() noexcept {
        return true;
    }
};

struct RustAdapter {
    RustResourceLedger ledger{};

    bool configure() noexcept {
        bool result = ledger.valid();
        for (std::size_t index = 0U; index < resource_class_count; ++index) {
            result = ledger.set_hard_limit(
                         static_cast<ResourceClass>(index),
                         256U * 1'024U + index * 4'096U) &&
                result;
        }
        return result;
    }

    bool reserve(ResourceClass resource_class, std::size_t bytes) noexcept {
        return ledger.try_reserve(resource_class, bytes);
    }
    bool release(ResourceClass resource_class, std::size_t bytes) noexcept {
        return ledger.release(resource_class, bytes);
    }
    bool cache_hit(ResourceClass resource_class) noexcept {
        return ledger.record_cache_hit(resource_class);
    }
    bool cache_miss(ResourceClass resource_class) noexcept {
        return ledger.record_cache_miss(resource_class);
    }
    bool eviction(ResourceClass resource_class) noexcept {
        return ledger.record_eviction(resource_class);
    }
    bool physical_read(ResourceClass resource_class, std::uint64_t bytes) noexcept {
        return ledger.record_physical_read(resource_class, bytes);
    }
    bool physical_write(ResourceClass resource_class, std::uint64_t bytes) noexcept {
        return ledger.record_physical_write(resource_class, bytes);
    }
    bool snapshot(ResourceClass resource_class, ResourceSnapshot& output) const noexcept {
        return ledger.snapshot(resource_class, output);
    }
    std::size_t total_current_bytes() const noexcept {
        return ledger.total_current_bytes();
    }
    std::size_t total_peak_bytes() const noexcept {
        return ledger.total_peak_bytes();
    }
    bool within_hard_limits() const noexcept {
        return ledger.within_hard_limits();
    }
    bool accounting_clean() const noexcept {
        return ledger.accounting_clean();
    }
    bool verify() noexcept {
        return ledger.valid();
    }
};

struct ShadowAdapter {
    ShadowResourceLedger ledger{4'096U};

    bool configure() noexcept {
        for (std::size_t index = 0U; index < resource_class_count; ++index) {
            ledger.set_hard_limit(
                static_cast<ResourceClass>(index),
                256U * 1'024U + index * 4'096U);
        }
        return ledger.healthy();
    }

    bool reserve(ResourceClass resource_class, std::size_t bytes) noexcept {
        return ledger.try_reserve(resource_class, bytes);
    }
    bool release(ResourceClass resource_class, std::size_t bytes) noexcept {
        ledger.release(resource_class, bytes);
        return true;
    }
    bool cache_hit(ResourceClass resource_class) noexcept {
        ledger.record_cache_hit(resource_class);
        return true;
    }
    bool cache_miss(ResourceClass resource_class) noexcept {
        ledger.record_cache_miss(resource_class);
        return true;
    }
    bool eviction(ResourceClass resource_class) noexcept {
        ledger.record_eviction(resource_class);
        return true;
    }
    bool physical_read(ResourceClass resource_class, std::uint64_t bytes) noexcept {
        ledger.record_physical_read(resource_class, bytes);
        return true;
    }
    bool physical_write(ResourceClass resource_class, std::uint64_t bytes) noexcept {
        ledger.record_physical_write(resource_class, bytes);
        return true;
    }
    bool snapshot(ResourceClass resource_class, ResourceSnapshot& output) const noexcept {
        output = ledger.snapshot(resource_class);
        return true;
    }
    std::size_t total_current_bytes() const noexcept {
        return ledger.total_current_bytes();
    }
    std::size_t total_peak_bytes() const noexcept {
        return ledger.total_peak_bytes();
    }
    bool within_hard_limits() const noexcept {
        return ledger.within_hard_limits();
    }
    bool accounting_clean() const noexcept {
        return ledger.accounting_clean();
    }
    bool verify() noexcept {
        return ledger.verify_now() && ledger.healthy();
    }
};

template <typename Adapter>
std::uint64_t replay(Adapter& adapter, const std::vector<Operation>& operations) noexcept {
    std::uint64_t checksum = 0x243F6A8885A308D3ULL;
    for (const Operation& operation : operations) {
        bool operation_valid = true;
        switch (operation.kind) {
        case OperationKind::Reserve: {
            const bool accepted =
                adapter.reserve(operation.resource_class, operation.bytes);
            checksum = mix(checksum, accepted ? 1U : 0U);
            continue;
        }
        case OperationKind::Release:
            operation_valid = adapter.release(operation.resource_class, operation.bytes);
            break;
        case OperationKind::CacheHit:
            operation_valid = adapter.cache_hit(operation.resource_class);
            break;
        case OperationKind::CacheMiss:
            operation_valid = adapter.cache_miss(operation.resource_class);
            break;
        case OperationKind::Eviction:
            operation_valid = adapter.eviction(operation.resource_class);
            break;
        case OperationKind::PhysicalRead:
            operation_valid =
                adapter.physical_read(operation.resource_class, operation.value);
            break;
        case OperationKind::PhysicalWrite:
            operation_valid =
                adapter.physical_write(operation.resource_class, operation.value);
            break;
        }
        if (!operation_valid) {
            checksum = mix(checksum, std::numeric_limits<std::uint64_t>::max());
        }
    }
    return checksum;
}

std::uint64_t snapshot_checksum(const ResourceSnapshot& snapshot) noexcept {
    std::uint64_t checksum = 0x13198A2E03707344ULL;
    checksum = mix(checksum, static_cast<std::uint64_t>(snapshot.hard_limit_bytes));
    checksum = mix(checksum, static_cast<std::uint64_t>(snapshot.current_bytes));
    checksum = mix(checksum, static_cast<std::uint64_t>(snapshot.peak_bytes));
    checksum = mix(checksum, snapshot.reservations);
    checksum = mix(checksum, snapshot.releases);
    checksum = mix(checksum, snapshot.rejected_reservations);
    checksum = mix(checksum, snapshot.accounting_errors);
    checksum = mix(checksum, snapshot.cache_hits);
    checksum = mix(checksum, snapshot.cache_misses);
    checksum = mix(checksum, snapshot.evictions);
    checksum = mix(checksum, snapshot.physical_read_bytes);
    checksum = mix(checksum, snapshot.physical_write_bytes);
    return checksum;
}

template <typename Adapter>
std::uint64_t finalize(const Adapter& adapter) noexcept {
    std::uint64_t checksum = 0U;
    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        ResourceSnapshot snapshot{};
        if (!adapter.snapshot(static_cast<ResourceClass>(index), snapshot)) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        checksum = mix(checksum, snapshot_checksum(snapshot));
    }
    checksum = mix(checksum, static_cast<std::uint64_t>(adapter.total_current_bytes()));
    checksum = mix(checksum, static_cast<std::uint64_t>(adapter.total_peak_bytes()));
    return checksum;
}

bool snapshots_equal(const ResourceSnapshot& left, const ResourceSnapshot& right) noexcept {
    return left.hard_limit_bytes == right.hard_limit_bytes &&
        left.current_bytes == right.current_bytes &&
        left.peak_bytes == right.peak_bytes &&
        left.reservations == right.reservations &&
        left.releases == right.releases &&
        left.rejected_reservations == right.rejected_reservations &&
        left.accounting_errors == right.accounting_errors &&
        left.cache_hits == right.cache_hits &&
        left.cache_misses == right.cache_misses &&
        left.evictions == right.evictions &&
        left.physical_read_bytes == right.physical_read_bytes &&
        left.physical_write_bytes == right.physical_write_bytes;
}

bool certify_equivalence(const std::vector<Operation>& operations) {
    CppAdapter cpp;
    RustAdapter rust;
    ShadowAdapter shadow;
    if (!cpp.configure() || !rust.configure() || !shadow.configure()) {
        return false;
    }

    const std::uint64_t cpp_result = replay(cpp, operations);
    const std::uint64_t rust_result = replay(rust, operations);
    const std::uint64_t shadow_result = replay(shadow, operations);
    if (cpp_result != rust_result || cpp_result != shadow_result ||
        !cpp.verify() || !rust.verify() || !shadow.verify()) {
        return false;
    }

    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        const auto resource_class = static_cast<ResourceClass>(index);
        ResourceSnapshot cpp_snapshot{};
        ResourceSnapshot rust_snapshot{};
        ResourceSnapshot shadow_snapshot{};
        if (!cpp.snapshot(resource_class, cpp_snapshot) ||
            !rust.snapshot(resource_class, rust_snapshot) ||
            !shadow.snapshot(resource_class, shadow_snapshot) ||
            !snapshots_equal(cpp_snapshot, rust_snapshot) ||
            !snapshots_equal(cpp_snapshot, shadow_snapshot)) {
            return false;
        }
    }

    return cpp.total_current_bytes() == rust.total_current_bytes() &&
        cpp.total_current_bytes() == shadow.total_current_bytes() &&
        cpp.total_peak_bytes() == rust.total_peak_bytes() &&
        cpp.total_peak_bytes() == shadow.total_peak_bytes() &&
        cpp.within_hard_limits() == rust.within_hard_limits() &&
        cpp.accounting_clean() == rust.accounting_clean();
}

template <typename Adapter>
double measure(const std::vector<Operation>& operations) {
    Adapter adapter;
    if (!adapter.configure()) {
        return std::numeric_limits<double>::infinity();
    }
    const auto begin = std::chrono::steady_clock::now();
    const std::uint64_t replay_checksum = replay(adapter, operations);
    const auto end = std::chrono::steady_clock::now();
    if (!adapter.verify()) {
        return std::numeric_limits<double>::infinity();
    }
    benchmark_sink.fetch_xor(
        mix(replay_checksum, finalize(adapter)),
        std::memory_order_relaxed);
    const double nanoseconds =
        std::chrono::duration<double, std::nano>(end - begin).count();
    return nanoseconds / static_cast<double>(operations.size());
}

double percentile(std::vector<double> samples, double quantile) {
    std::sort(samples.begin(), samples.end());
    const double rank = std::ceil(quantile * static_cast<double>(samples.size()));
    const std::size_t index = static_cast<std::size_t>(rank) - 1U;
    return samples[std::min(index, samples.size() - 1U)];
}

Metrics summarize(const std::vector<double>& samples) {
    Metrics metrics{};
    metrics.p50 = percentile(samples, 0.50);
    metrics.p95 = percentile(samples, 0.95);
    metrics.p99 = percentile(samples, 0.99);
    metrics.maximum = *std::max_element(samples.begin(), samples.end());
    metrics.operations_per_second = 1'000'000'000.0 / metrics.p50;
    return metrics;
}

void write_metrics(std::ostream& output, const Metrics& metrics) {
    output << "{\"p50_ns_per_operation\":" << metrics.p50
           << ",\"p95_ns_per_operation\":" << metrics.p95
           << ",\"p99_ns_per_operation\":" << metrics.p99
           << ",\"max_ns_per_operation\":" << metrics.maximum
           << ",\"operations_per_second\":" << metrics.operations_per_second << '}';
}

bool write_report(
    const std::string& path,
    std::size_t operation_count,
    std::size_t sample_count,
    const Metrics& cpp,
    const Metrics& rust,
    const Metrics& shadow,
    double rust_ratio,
    double shadow_ratio) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << std::fixed << std::setprecision(3)
           << "{\"schema\":\"zevryon.ledger-performance.v1\","
           << "\"operations_per_sample\":" << operation_count << ','
           << "\"samples\":" << sample_count << ",\"cpp\":";
    write_metrics(output, cpp);
    output << ",\"rust\":";
    write_metrics(output, rust);
    output << ",\"shadow\":";
    write_metrics(output, shadow);
    output << ",\"rust_to_cpp_p50_ratio\":" << rust_ratio
           << ",\"shadow_to_cpp_p50_ratio\":" << shadow_ratio
           << ",\"sink\":" << benchmark_sink.load(std::memory_order_relaxed) << "}\n";
    return static_cast<bool>(output);
}

} // namespace

int main(int argc, char** argv) {
    constexpr std::size_t operation_count = 300'000U;
    constexpr std::size_t sample_count = 13U;
    std::string report_path = "z2r1-ledger-performance.json";
    if (argc == 3 && std::string(argv[1]) == "--json") {
        report_path = argv[2];
    } else if (argc != 1) {
        std::cerr << "usage: zevryon-rust-resource-ledger-benchmark [--json PATH]\n";
        return 2;
    }

    const std::vector<Operation> operations = generate_workload(operation_count);
    if (!certify_equivalence(operations)) {
        std::cerr << "FAILED: correctness certification before timing\n";
        return 1;
    }

    static_cast<void>(measure<CppAdapter>(operations));
    static_cast<void>(measure<RustAdapter>(operations));
    static_cast<void>(measure<ShadowAdapter>(operations));

    std::vector<double> cpp_samples;
    std::vector<double> rust_samples;
    std::vector<double> shadow_samples;
    cpp_samples.reserve(sample_count);
    rust_samples.reserve(sample_count);
    shadow_samples.reserve(sample_count);

    for (std::size_t sample = 0U; sample < sample_count; ++sample) {
        switch (sample % 3U) {
        case 0U:
            cpp_samples.push_back(measure<CppAdapter>(operations));
            rust_samples.push_back(measure<RustAdapter>(operations));
            shadow_samples.push_back(measure<ShadowAdapter>(operations));
            break;
        case 1U:
            rust_samples.push_back(measure<RustAdapter>(operations));
            shadow_samples.push_back(measure<ShadowAdapter>(operations));
            cpp_samples.push_back(measure<CppAdapter>(operations));
            break;
        default:
            shadow_samples.push_back(measure<ShadowAdapter>(operations));
            cpp_samples.push_back(measure<CppAdapter>(operations));
            rust_samples.push_back(measure<RustAdapter>(operations));
            break;
        }
    }

    const Metrics cpp = summarize(cpp_samples);
    const Metrics rust = summarize(rust_samples);
    const Metrics shadow = summarize(shadow_samples);
    const double cpp_floor = std::max(cpp.p50, 0.001);
    const double rust_ratio = rust.p50 / cpp_floor;
    const double shadow_ratio = shadow.p50 / cpp_floor;

    if (!write_report(
            report_path,
            operation_count,
            sample_count,
            cpp,
            rust,
            shadow,
            rust_ratio,
            shadow_ratio)) {
        std::cerr << "FAILED: unable to write benchmark report\n";
        return 1;
    }

    std::cout << std::fixed << std::setprecision(3)
              << "C++ p50=" << cpp.p50 << " ns/op, Rust p50=" << rust.p50
              << " ns/op, shadow p50=" << shadow.p50 << " ns/op, Rust/C++="
              << rust_ratio << "x, shadow/C++=" << shadow_ratio << "x\n";

    const double rust_p50_limit = std::max(cpp.p50 * 20.0, cpp.p50 + 200.0);
    const double rust_p99_limit = std::max(cpp.p99 * 30.0, cpp.p99 + 1'000.0);
    const double shadow_p50_limit = std::max(
        (cpp.p50 + rust.p50) * 5.0,
        cpp.p50 + rust.p50 + 500.0);

    if (!std::isfinite(cpp.p50) || !std::isfinite(rust.p50) ||
        !std::isfinite(shadow.p50) || rust.p50 > rust_p50_limit ||
        rust.p99 > rust_p99_limit || shadow.p50 > shadow_p50_limit ||
        cpp.operations_per_second < 100'000.0 ||
        rust.operations_per_second < 100'000.0 ||
        shadow.operations_per_second < 100'000.0) {
        std::cerr << "FAILED: catastrophic ledger performance regression gate\n";
        return 1;
    }

    std::cout << "Resource ledger performance oracle passed\n";
    return 0;
}
