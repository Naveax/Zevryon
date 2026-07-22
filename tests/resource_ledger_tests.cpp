#include "resource_ledger.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

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
            "JSON exposes hard-limit status")) {
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

    std::cout << "Resource ledger tests passed\n";
    return 0;
}
