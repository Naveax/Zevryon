#include "resource_ledger.hpp"
#include "rust_resource_ledger.hpp"
#include "shadow_resource_ledger.hpp"

#include <algorithm>
#include <array>
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
    double p50_ns_per_operation{0.0};
    double p95_ns_per_operation{0.0};
    double p99_ns_per_operation{0.0};
    double max_ns_per_operation{0.0};
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

std::vector<Operation> generate_workload(std::size_t operation_count) {
    std::vector<Operation> operations;
    operations.reserve(operation_count);
    std::uint64_t state = 0xBB67AE8584CAA73BULL;

    for (std::size_t index = 0U; index < operation_count; ++index) {
        const std::uint64_t random = next_random(state);
        const std::size_t class_index = static_cast<std::size_t>(
            random % static_cast<std::uint64_t>(resource_class_count));
        Operation operation{};
        operation.resource_class = static_cast<ResourceClass>(class_index);
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

void configure(ResourceLedger& ledger) noexcept {
    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        ledger.set_hard_limit(
            static_cast<ResourceClass>(index),
            256U * 1'024U + index * 4'096U);
    }
}

bool configure(RustResourceLedger& ledger) noexcept {
    bool configured = ledger.valid();
    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        configured = ledger.set_hard_limit(
                         static_cast<ResourceClass>(index),
                         256U * 1'024U + index * 4'096U) &&
            configured;
    }
    return configured;
}

void configure(ShadowResourceLedger& ledger) noexcept {
    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        ledger.set_hard_limit(
            static_cast<ResourceClass>(index),
            256U * 1'024U + index * 4'096U);
    }
}

std::uint64_t replay(ResourceLedger& ledger, const std::vector<Operation>& operations) noexcept {
    std::uint64_t checksum = 0x243F6A8885A308D3ULL;
    for (const Operation& operation : operations) {
        switch (operation.kind) {
        case OperationKind::Reserve:
            checksum = mix(
                checksum,
                ledger.try_reserve(operation.resource_class, operation.bytes) ? 1U : 0U);
            break;
        case OperationKind::Release:
            ledger.release(operation.resource_class, operation.bytes);
            break;
        case OperationKind::CacheHit:
            ledger.record_cache_hit(operation.resource_class);
            break;
        case OperationKind::CacheMiss:
            ledger.record_cache_miss(operation.resource_class);
            break;
        case OperationKind::Eviction:
            ledger.record_eviction(operation.resource_class);
            break;
        case OperationKind::PhysicalRead:
            ledger.record_physical_read(operation.resource_class, operation.value);
            break;
        case OperationKind::PhysicalWrite:
            ledger.record_physical_write(operation.resource_class, operation.value);
            break;
        }
    }
    return checksum;
}

std::uint64_t replay(
    RustResourceLedger& ledger,
    const std::vector<Operation>& operations) noexcept {
    std::uint64_t checksum = 0x243F6A8885A308D3ULL;
    for (const Operation& operation : operations) {
        bool result = true;
        switch (operation.kind) {
        case OperationKind::Reserve:
            result = ledger.try_reserve(operation.resource_class, operation.bytes);
            checksum = mix(checksum, result ? 1U : 0U);
            break;
        case OperationKind::Release:
            result = ledger.release(operation.resource_class, operation.bytes);
            break;
        case OperationKind::CacheHit:
            result = ledger.record_cache_hit(operation.resource_class);
            break;
        case OperationKind::CacheMiss:
            result = ledger.record_cache_miss(operation.resource_class);
            break;
        case OperationKind::Eviction:
            result = ledger.record_eviction(operation.resource_class);
            break;
        case OperationKind::PhysicalRead:
            result = ledger.record_physical_read(operation.resource_class, operation.value);
            break;
        case OperationKind::PhysicalWrite:
            result = ledger.record_physical_write(operation.resource_class, operation.value);
            break;
        }
        if (!result) {
            checksum = mix(checksum, std::numeric_limits<std::uint64_t>::max());
        }
    }
    return checksum;
}

std::uint64_t replay(
    ShadowResourceLedger& ledger,
    const std::vector<Operation>& operations) noexcept {
    std::uint64_t checksum = 0x243F6A8885A308D3ULL;
    for (const Operation& operation : operations) {
        switch (operation.kind) {
        case OperationKind::Reserve:
            checksum = mix(
                checksum,
                ledger.try_reserve(operation.resource_class, operation.bytes) ? 1U : 0U);
            break;
        case OperationKind::Release:
            ledger.release(operation.resource_class, operation.bytes);
            break;
        case OperationKind::CacheHit:
            ledger.record_cache_hit(operation.resource_class);
            break;
        case OperationKind::CacheMiss:
            ledger.record_cache_miss(operation.resource_class);
            break;
        case OperationKind::Eviction:
            ledger.record_eviction(operation.resource_class);
            break;
        case OperationKind::PhysicalRead:
            ledger.record_physical_read(operation.resource_class, operation.value);
            break;
        case OperationKind::PhysicalWrite:
            ledger.record_physical_write(operation.resource_class, operation.value);
            break;
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

std::uint64_t finalize(const ResourceLedger& ledger) noexcept {
    std::uint64_t checksum = 0U;
    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        checksum = mix(
            checksum,
            snapshot_checksum(ledger.snapshot(static_cast<ResourceClass>(index))));
    }
    checksum = mix(checksum, static_cast<std::uint64_t>(ledger.total_current_bytes()));
    checksum = mix(checksum, static_cast<std::uint64_t>(ledger.total_peak_bytes()));
    return checksum;
}

std::uint64_t finalize(const RustResourceLedger& ledger) noexcept {
    std::uint64_t checksum = 0U;
    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        ResourceSnapshot snapshot{};
        if (!ledger.snapshot(static_cast<ResourceClass>(index), snapshot)) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        checksum = mix(checksum, snapshot_checksum(snapshot));
    }
    checksum = mix(checksum, static_cast<std::uint64_t>(ledger.total_current_bytes()));
    checksum = mix(checksum, static_cast<std::uint64_t>(ledger.total_peak_bytes()));
    return checksum;
}

std::uint64_t finalize(const ShadowResourceLedger& ledger) noexcept {
    std::uint64_t checksum = 0U;
    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        checksum = mix(
            checksum,
            snapshot_checksum(ledger.snapshot(static_cast<ResourceClass>(index))));
    }
    checksum = mix(checksum, static_cast<std::uint64_t>(ledger.total_current_bytes()));
    checksum = mix(checksum, static_cast<std::uint64_t>(ledger.total_peak_bytes()));
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
    ResourceLedger cpp_ledger;
    RustResourceLedger rust_ledger;
    ShadowResourceLedger shadow_ledger(4'096U);
    configure(cpp_ledger);
    if (!configure(rust_ledger)) {
        return false;
    }
    configure(shadow_ledger);

    const std::uint64_t cpp_result = replay(cpp_ledger, operations);
    const std::uint64_t rust_result = replay(rust_ledger, operations);
    const std::uint64_t shadow_result = replay(shadow_ledger, operations);
    if (cpp_result != rust_result || cpp_result != shadow_result ||
        !shadow_ledger.verify_now() || !shadow_ledger.healthy()) {
        return false;
    }

    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        const auto resource_class = static_cast<ResourceClass>(index);
        ResourceSnapshot rust_snapshot{};
        if (!rust_ledger.snapshot(resource_class, rust_snapshot) ||
            !snapshots_equal(cpp_ledger.snapshot(resource_class), rust_snapshot) ||
            !snapshots_equal(cpp_ledger.snapshot(resource_class), shadow_ledger.snapshot(resource_class))) {
            return false;
        }
    }

    return cpp_ledger.total_current_bytes() == rust_ledger.total_current_bytes() &&
        cpp_ledger.total_current_bytes() == shadow_ledger.total_current_bytes() &&
        cpp_ledger.total_peak_bytes() == rust_ledger.total_peak_bytes() &&
        cpp_ledger.total_peak_bytes() == shadow_ledger.total_peak_bytes() &&
        cpp_ledger.within_hard_limits() == rust_ledger.within_hard_limits() &&
        cpp_ledger.accounting_clean() == rust_ledger.accounting_clean();
}

double measure_cpp(const std::vector<Operation>& operations) {
    ResourceLedger ledger;
    configure(ledger);
    const auto begin = std::chrono::steady_clock::now();
    const std::uint64_t replay_checksum = replay(ledger, operations);
    const auto end = std::chrono::steady_clock::now();
    benchmark_sink.fetch_xor(mix(replay_checksum, finalize(ledger)), std::memory_order_relaxed);
    const double nanoseconds =
        std::chrono::duration<double, std::nano>(end - begin).count();
    return nanoseconds / static_cast<double>(operations.size());
}

double measure_rust(const std::vector<Operation>& operations) {
    RustResourceLedger ledger;
    if (!configure(ledger)) {
        return std::numeric_limits<double>::infinity();
    }
    const auto begin = std::chrono::steady_clock::now();
    const std::uint64_t replay_checksum = replay(ledger, operations);
    const auto end = std::chrono::steady_clock::now();
    benchmark_sink.fetch_xor(mix(replay_checksum, finalize(ledger)), std::memory_order_relaxed);
    const double nanoseconds =
        std::chrono::duration<double, std::nano>(end - begin).count();
    return nanoseconds / static_cast<double>(operations.size());
}

double measure_shadow(const std::vector<Operation>& operations) {
    ShadowResourceLedger ledger(4'096U);
    configure(ledger);
    const auto begin = std::chrono::steady_clock::now();
    const std::uint64_t replay_checksum = replay(ledger, operations);
    const auto end = std::chrono::steady_clock::now();
    if (!ledger.verify_now() || !ledger.healthy()) {
        return std::numeric_limits<double>::infinity();
    }
    benchmark_sink.fetch_xor(mix(replay_checksum, finalize(ledger)), std::memory_order_relaxed);
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
    metrics.p50_ns_per_operation = percentile(samples, 0.50);
    metrics.p95_ns_per_operation = percentile(samples, 0.95);
    metrics.p99_ns_per_operation = percentile(samples, 0.99);
    metrics.max_ns_per_operation = *std::max_element(samples.begin(), samples.end());
    metrics.operations_per_second = 1'000'000'000.0 / metrics.p50_ns_per_operation;
    return metrics;
}

void write_metrics(std::ostream& output, const Metrics& metrics) {
    output << "{\"p50_ns_per_operation\":" << metrics.p50_ns_per_operation
           << ",\"p95_ns_per_operation\":" << metrics.p95_ns_per_operation
           << ",\"p99_ns_per_operation\":" << metrics.p99_ns_per_operation
           << ",\"max_ns_per_operation\":" << metrics.max_ns_per_operation
           << ",\"operations_per_second\":" << metrics.operations_per_second << '}';
}

bool write_report(
    const std::string& path,
    std::size_t operation_count,
    std::size_t sample_count,
    const Metrics& cpp_metrics,
    const Metrics& rust_metrics,
    const Metrics& shadow_metrics,
    double rust_ratio,
    double shadow_ratio) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << std::fixed << std::setprecision(3);
    output << "{\"schema\":\"zevryon.ledger-performance.v1\","
           << "\"operations_per_sample\":" << operation_count << ','
           << "\"samples\":" << sample_count << ",\"cpp\":";
    write_metrics(output, cpp_metrics);
    output << ",\"rust\":";
    write_metrics(output, rust_metrics);
    output << ",\"shadow\":";
    write_metrics(output, shadow_metrics);
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

    static_cast<void>(measure_cpp(operations));
    static_cast<void>(measure_rust(operations));
    static_cast<void>(measure_shadow(operations));

    std::vector<double> cpp_samples;
    std::vector<double> rust_samples;
    std::vector<double> shadow_samples;
    cpp_samples.reserve(sample_count);
    rust_samples.reserve(sample_count);
    shadow_samples.reserve(sample_count);

    for (std::size_t sample = 0U; sample < sample_count; ++sample) {
        switch (sample % 3U) {
        case 0U:
            cpp_samples.push_back(measure_cpp(operations));
            rust_samples.push_back(measure_rust(operations));
            shadow_samples.push_back(measure_shadow(operations));
            break;
        case 1U:
            rust_samples.push_back(measure_rust(operations));
            shadow_samples.push_back(measure_shadow(operations));
            cpp_samples.push_back(measure_cpp(operations));
            break;
        default:
            shadow_samples.push_back(measure_shadow(operations));
            cpp_samples.push_back(measure_cpp(operations));
            rust_samples.push_back(measure_rust(operations));
            break;
        }
    }

    const Metrics cpp_metrics = summarize(cpp_samples);
    const Metrics rust_metrics = summarize(rust_samples);
    const Metrics shadow_metrics = summarize(shadow_samples);
    const double cpp_floor = std::max(cpp_metrics.p50_ns_per_operation, 0.001);
    const double rust_ratio = rust_metrics.p50_ns_per_operation / cpp_floor;
    const double shadow_ratio = shadow_metrics.p50_ns_per_operation / cpp_floor;

    if (!write_report(
            report_path,
            operation_count,
            sample_count,
            cpp_metrics,
            rust_metrics,
            shadow_metrics,
            rust_ratio,
            shadow_ratio)) {
        std::cerr << "FAILED: unable to write benchmark report\n";
        return 1;
    }

    std::cout << std::fixed << std::setprecision(3)
              << "C++ p50=" << cpp_metrics.p50_ns_per_operation
              << " ns/op, Rust p50=" << rust_metrics.p50_ns_per_operation
              << " ns/op, shadow p50=" << shadow_metrics.p50_ns_per_operation
              << " ns/op, Rust/C++=" << rust_ratio
              << "x, shadow/C++=" << shadow_ratio << "x\n";

    const double rust_p50_limit = std::max(
        cpp_metrics.p50_ns_per_operation * 20.0,
        cpp_metrics.p50_ns_per_operation + 200.0);
    const double rust_p99_limit = std::max(
        cpp_metrics.p99_ns_per_operation * 30.0,
        cpp_metrics.p99_ns_per_operation + 1'000.0);
    const double shadow_p50_limit = std::max(
        (cpp_metrics.p50_ns_per_operation + rust_metrics.p50_ns_per_operation) * 5.0,
        cpp_metrics.p50_ns_per_operation + rust_metrics.p50_ns_per_operation + 500.0);

    if (rust_metrics.p50_ns_per_operation > rust_p50_limit ||
        rust_metrics.p99_ns_per_operation > rust_p99_limit ||
        shadow_metrics.p50_ns_per_operation > shadow_p50_limit ||
        cpp_metrics.operations_per_second < 100'000.0 ||
        rust_metrics.operations_per_second < 100'000.0 ||
        shadow_metrics.operations_per_second < 100'000.0) {
        std::cerr << "FAILED: catastrophic ledger performance regression gate\n";
        return 1;
    }

    std::cout << "Resource ledger performance oracle passed\n";
    return 0;
}
