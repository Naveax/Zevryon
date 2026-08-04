#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

int run_positive() {
    using zevryon::core::LedgerMemoryResource;
    using zevryon::core::ResourceClass;
    using zevryon::core::ResourceLedger;
    using zevryon::core::resource_class_count;

    if (!require(ResourceLedger::rust_authoritative(), "Rust authority compile-time identity") ||
        !require(
            std::string_view(ResourceLedger::authoritative_backend()) == "rust",
            "Rust is the published authoritative backend")) {
        return 1;
    }

    ResourceLedger ledger;
    if (!require(ledger.rust_shadow_enabled(), "Rust authority storage initializes") ||
        !require(ledger.rust_shadow_healthy(), "fresh authority ledger is healthy")) {
        return 1;
    }

    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        const auto resource_class = static_cast<ResourceClass>(index);
        const std::size_t bytes = 64U + index;
        ledger.set_hard_limit(resource_class, bytes);
        if (!require(ledger.try_reserve(resource_class, bytes), "exact hard limit reserves") ||
            !require(
                !ledger.try_reserve(resource_class, 1U),
                "Rust authority rejects one byte beyond the hard limit")) {
            return 1;
        }
        ledger.record_cache_hit(resource_class);
        ledger.record_cache_miss(resource_class);
        ledger.record_eviction(resource_class);
        ledger.record_physical_read(resource_class, static_cast<std::uint64_t>(bytes));
        ledger.record_physical_write(
            resource_class,
            static_cast<std::uint64_t>(bytes / 2U));

        const auto snapshot = ledger.snapshot(resource_class);
        if (!require(snapshot.current_bytes == bytes, "snapshot is sourced from Rust") ||
            !require(snapshot.reservations == 1U, "accepted reservation is counted") ||
            !require(
                snapshot.rejected_reservations == 1U,
                "rejected reservation is counted") ||
            !require(snapshot.cache_hits == 1U, "cache hit is counted") ||
            !require(snapshot.cache_misses == 1U, "cache miss is counted") ||
            !require(snapshot.evictions == 1U, "eviction is counted")) {
            return 1;
        }
        ledger.release(resource_class, bytes);
    }

    ledger.record_physical_read(
        ResourceClass::NetworkBuffer,
        std::numeric_limits<std::uint64_t>::max() - 2U);
    ledger.record_physical_read(ResourceClass::NetworkBuffer, 8U);
    if (!require(
            ledger.snapshot(ResourceClass::NetworkBuffer).physical_read_bytes ==
                std::numeric_limits<std::uint64_t>::max(),
            "Rust authority saturates physical I/O counters")) {
        return 1;
    }

    ResourceLedger pmr_ledger;
    pmr_ledger.set_hard_limit(ResourceClass::LayoutFragment, 16'384U);
    {
        LedgerMemoryResource memory(
            pmr_ledger,
            ResourceClass::LayoutFragment,
            std::pmr::get_default_resource());
        std::pmr::vector<std::byte> allocation(&memory);
        allocation.resize(4'096U);
        if (!require(
                pmr_ledger.snapshot(ResourceClass::LayoutFragment).current_bytes >= 4'096U,
                "real PMR allocation is decided and reported by Rust")) {
            return 1;
        }
    }

    if (!require(
            pmr_ledger.snapshot(ResourceClass::LayoutFragment).current_bytes == 0U,
            "real PMR deallocation returns Rust authority to zero") ||
        !require(ledger.total_current_bytes() == 0U, "all class reservations release") ||
        !require(ledger.within_hard_limits(), "Rust authority remains within limits") ||
        !require(ledger.accounting_clean(), "Rust authority accounting remains clean") ||
        !require(ledger.verify_rust_shadow(), "C++ reverse shadow exactly matches Rust") ||
        !require(ledger.rust_shadow_mismatches() == 0U, "no reverse-shadow mismatch") ||
        !require(ledger.rust_shadow_verifications() >= 1U, "verification is counted")) {
        return 1;
    }

    const std::string telemetry = ledger.rust_shadow_json();
    const std::string report = ledger.json();
    if (!require(
            telemetry.find("\"mode\":\"rust_authoritative\"") != std::string::npos,
            "telemetry identifies authority mode") ||
        !require(
            telemetry.find("\"authoritative_backend\":\"rust\"") !=
                std::string::npos,
            "telemetry identifies Rust authority") ||
        !require(
            telemetry.find("\"shadow_backend\":\"cpp\"") != std::string::npos,
            "telemetry identifies C++ reverse shadow") ||
        !require(
            report.find("\"authoritative_backend\":\"rust\"") !=
                std::string::npos,
            "resource report publishes Rust authority") ||
        !require(
            report.find("\"verification_backend\":\"cpp\"") !=
                std::string::npos,
            "resource report publishes C++ verifier")) {
        return 1;
    }

    std::cout << "Rust-authoritative ResourceLedger positive certification passed\n";
    return 0;
}

int run_fault() {
    using zevryon::core::ResourceClass;
    using zevryon::core::ResourceLedger;

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::LayoutFragment, 1'024U);
    if (!require(
            ledger.try_reserve(ResourceClass::LayoutFragment, 64U),
            "authority fixture reserves bytes") ||
        !require(
            ledger.snapshot(ResourceClass::LayoutFragment).current_bytes == 64U,
            "pre-fault Rust snapshot is correct")) {
        return 1;
    }

    ledger.inject_cpp_shadow_reservation_for_testing(
        ResourceClass::LayoutFragment,
        1U);

    if (!require(
            ledger.snapshot(ResourceClass::LayoutFragment).current_bytes == 64U,
            "public snapshot remains Rust-authoritative after C++ corruption") ||
        !require(
            ledger.total_current_bytes() == 64U,
            "public aggregate remains Rust-authoritative after C++ corruption") ||
        !require(
            !ledger.verify_rust_shadow(),
            "reverse-shadow verification detects injected C++ divergence") ||
        !require(
            ledger.rust_shadow_mismatches() >= 1U,
            "divergence increments the mismatch counter") ||
        !require(
            !ledger.rust_shadow_healthy(),
            "divergence latches unhealthy telemetry")) {
        return 1;
    }

    const std::string telemetry = ledger.rust_shadow_json();
    if (!require(
            telemetry.find("\"authoritative_backend\":\"rust\"") !=
                std::string::npos,
            "fault telemetry retains Rust authority") ||
        !require(
            telemetry.find("\"shadow_backend\":\"cpp\"") != std::string::npos,
            "fault telemetry identifies the divergent C++ verifier") ||
        !require(
            telemetry.find("\"first_field\":\"current_bytes\"") !=
                std::string::npos,
            "fault telemetry identifies the first divergent field") ||
        !require(
            telemetry.find("\"expected\":64") != std::string::npos,
            "fault telemetry records the Rust-authoritative value") ||
        !require(
            telemetry.find("\"actual\":65") != std::string::npos,
            "fault telemetry records the corrupted C++ value")) {
        return 1;
    }

    std::cout << "Rust-authoritative reverse-shadow fault certification passed\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: zevryon-rust-authority-tests --positive|--fault\n";
        return 2;
    }

    const std::string_view mode(argv[1]);
    if (mode == "--positive") {
        return run_positive();
    }
    if (mode == "--fault") {
        return run_fault();
    }

    std::cerr << "unknown mode: " << mode << '\n';
    return 2;
}
