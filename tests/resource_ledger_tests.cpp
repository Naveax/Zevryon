#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main() {
    using zevryon::core::LedgerMemoryResource;
    using zevryon::core::ResourceClass;
    using zevryon::core::ResourceLedger;

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::SourceWindow, 100U);
    ledger.set_hard_limit(ResourceClass::CheckpointIndex, 40U);

    if (!require(
            ledger.try_reserve(ResourceClass::SourceWindow, 60U),
            "first reservation fits") ||
        !require(
            !ledger.try_reserve(ResourceClass::SourceWindow, 50U),
            "reservation above remaining hard cap is rejected") ||
        !require(
            ledger.try_reserve(ResourceClass::SourceWindow, 40U),
            "reservation exactly filling hard cap fits")) {
        return 1;
    }

    ledger.record_cache_hit(ResourceClass::SourceWindow);
    ledger.record_cache_miss(ResourceClass::SourceWindow);
    ledger.record_eviction(ResourceClass::SourceWindow);
    ledger.record_physical_read(ResourceClass::SourceWindow, 65'536U);
    ledger.record_physical_write(ResourceClass::SourceWindow, 4'096U);

    const auto full = ledger.snapshot(ResourceClass::SourceWindow);
    if (!require(full.current_bytes == 100U, "current bytes reach hard cap") ||
        !require(full.peak_bytes == 100U, "peak bytes record hard cap") ||
        !require(full.reservations == 2U, "successful reservations counted") ||
        !require(full.rejected_reservations == 1U, "rejected reservation counted") ||
        !require(full.cache_hits == 1U, "cache hit counted") ||
        !require(full.cache_misses == 1U, "cache miss counted") ||
        !require(full.evictions == 1U, "eviction counted") ||
        !require(full.physical_read_bytes == 65'536U, "physical reads counted") ||
        !require(full.physical_write_bytes == 4'096U, "physical writes counted") ||
        !require(ledger.total_current_bytes() == 100U, "total current bytes match") ||
        !require(ledger.total_peak_bytes() == 100U, "total peak bytes match") ||
        !require(ledger.within_hard_limits(), "ledger remains inside hard limits") ||
        !require(ledger.accounting_clean(), "valid operations keep accounting clean")) {
        return 1;
    }

    ledger.release(ResourceClass::SourceWindow, 40U);
    ledger.release(ResourceClass::SourceWindow, 60U);
    if (!require(ledger.total_current_bytes() == 0U, "all reservations release cleanly") ||
        !require(
            ledger.snapshot(ResourceClass::SourceWindow).releases == 2U,
            "releases counted")) {
        return 1;
    }

    if (!require(
            ledger.try_reserve(ResourceClass::CheckpointIndex, 40U),
            "independent resource class uses independent hard cap") ||
        !require(
            !ledger.try_reserve(ResourceClass::CheckpointIndex, 1U),
            "checkpoint budget rejects overflow")) {
        return 1;
    }
    ledger.release(ResourceClass::CheckpointIndex, 40U);

    ledger.record_physical_read(
        ResourceClass::NetworkBuffer,
        std::numeric_limits<std::uint64_t>::max() - 2U);
    ledger.record_physical_read(ResourceClass::NetworkBuffer, 8U);
    if (!require(
            ledger.snapshot(ResourceClass::NetworkBuffer).physical_read_bytes ==
                std::numeric_limits<std::uint64_t>::max(),
            "I/O counters saturate instead of wrapping")) {
        return 1;
    }

    const std::string json = ledger.json();
    if (!require(
            json.find("zevryon.resource-ledger.v1") != std::string::npos,
            "JSON exposes schema") ||
        !require(
            json.find("\"source_window\"") != std::string::npos,
            "JSON exposes source window resource") ||
        !require(
            json.find("\"within_hard_limits\":true") != std::string::npos,
            "JSON exposes hard-limit status") ||
        !require(
            json.find("zevryon.rust-shadow-ledger.v1") != std::string::npos,
            "JSON embeds the Rust shadow telemetry schema")) {
        return 1;
    }

    ResourceLedger broken;
    broken.set_hard_limit(ResourceClass::LayoutFragment, 32U);
    if (!require(
            broken.try_reserve(ResourceClass::LayoutFragment, 16U),
            "accounting-error fixture reserves bytes")) {
        return 1;
    }
    broken.release(ResourceClass::LayoutFragment, 17U);
    if (!require(!broken.accounting_clean(), "over-release is visible as accounting error") ||
        !require(
            broken.snapshot(ResourceClass::LayoutFragment).current_bytes == 0U,
            "over-release fails closed")) {
        return 1;
    }

    ResourceLedger production;
    production.set_hard_limit(ResourceClass::LayoutFragment, 8'192U);
    {
        LedgerMemoryResource memory(
            production,
            ResourceClass::LayoutFragment,
            std::pmr::get_default_resource());
        std::pmr::vector<std::byte> bytes(&memory);
        bytes.resize(2'048U);
        if (!require(
                production.snapshot(ResourceClass::LayoutFragment).current_bytes >= 2'048U,
                "production PMR allocation is accounted")) {
            return 1;
        }
    }
    if (!require(
            production.snapshot(ResourceClass::LayoutFragment).current_bytes == 0U,
            "production PMR deallocation releases its reservation")) {
        return 1;
    }

#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    if (!require(production.rust_shadow_enabled(), "production Rust shadow initializes") ||
        !require(
            production.rust_shadow_operations() >= 3U,
            "production PMR path reaches the embedded Rust shadow") ||
        !require(production.verify_rust_shadow(), "manual production shadow verification passes") ||
        !require(production.rust_shadow_healthy(), "production shadow remains healthy") ||
        !require(
            production.rust_shadow_verifications() >= 1U,
            "production shadow verification is counted") ||
        !require(
            production.rust_shadow_mismatches() == 0U,
            "production shadow records no mismatch") ||
        !require(
            production.rust_shadow_json().find("\"enabled\":true") !=
                std::string::npos,
            "production telemetry reports the Rust shadow enabled")) {
        return 1;
    }
#else
    if (!require(!production.rust_shadow_enabled(), "default C++ build keeps Rust disabled") ||
        !require(
            production.rust_shadow_operations() == 0U,
            "default C++ build has zero Rust operations") ||
        !require(
            production.rust_shadow_json().find("\"enabled\":false") !=
                std::string::npos,
            "default telemetry reports the Rust shadow disabled")) {
        return 1;
    }
#endif

    std::cout << "Resource ledger tests passed\n";
    return 0;
}
