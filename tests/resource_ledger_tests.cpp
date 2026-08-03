#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zevryon::core::LedgerMemoryResource;
using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;
using zevryon::core::ResourceSnapshot;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

void checksum_byte(std::uint64_t& checksum, std::uint8_t value) noexcept {
    checksum ^= value;
    checksum *= 1'099'511'628'211ULL;
}

void checksum_u64(std::uint64_t& checksum, std::uint64_t value) noexcept {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        checksum_byte(checksum, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void checksum_text(std::uint64_t& checksum, std::string_view text) noexcept {
    for (const char character : text) {
        checksum_byte(checksum, static_cast<std::uint8_t>(character));
    }
}

void checksum_snapshot(std::uint64_t& checksum, const ResourceSnapshot& snapshot) noexcept {
    checksum_u64(checksum, static_cast<std::uint64_t>(snapshot.hard_limit_bytes));
    checksum_u64(checksum, static_cast<std::uint64_t>(snapshot.current_bytes));
    checksum_u64(checksum, static_cast<std::uint64_t>(snapshot.peak_bytes));
    checksum_u64(checksum, snapshot.reservations);
    checksum_u64(checksum, snapshot.releases);
    checksum_u64(checksum, snapshot.rejected_reservations);
    checksum_u64(checksum, snapshot.accounting_errors);
    checksum_u64(checksum, snapshot.cache_hits);
    checksum_u64(checksum, snapshot.cache_misses);
    checksum_u64(checksum, snapshot.evictions);
    checksum_u64(checksum, snapshot.physical_read_bytes);
    checksum_u64(checksum, snapshot.physical_write_bytes);
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

template <std::size_t Count>
bool apply_workload_group(
    ResourceLedger& ledger,
    const char* name,
    const std::array<ResourceClass, Count>& classes,
    std::uint64_t& checksum,
    std::ostringstream& workloads,
    bool& first_workload) {
    const std::uint64_t operations_before = ledger.rust_shadow_operations();
    const std::uint64_t verifications_before = ledger.rust_shadow_verifications();
    checksum_text(checksum, name);

    for (std::size_t index = 0U; index < classes.size(); ++index) {
        const ResourceClass resource_class = classes[index];
        const std::size_t hard_limit = 1'048'576U + index * 16'384U;
        const std::size_t first_reservation = 8'192U + index * 512U;
        const std::size_t half_reservation = first_reservation / 2U;

        ledger.set_hard_limit(resource_class, hard_limit);
        if (!ledger.try_reserve(resource_class, first_reservation)) {
            std::cerr << "workload probe reservation failed for "
                      << zevryon::core::resource_class_name(resource_class) << '\n';
            return false;
        }
        ledger.record_cache_hit(resource_class);
        ledger.record_cache_miss(resource_class);
        ledger.record_eviction(resource_class);
        ledger.record_physical_read(
            resource_class,
            static_cast<std::uint64_t>(first_reservation) * 3U);
        ledger.record_physical_write(
            resource_class,
            static_cast<std::uint64_t>(first_reservation));
        ledger.release(resource_class, half_reservation);
        if (!ledger.try_reserve(resource_class, half_reservation)) {
            std::cerr << "workload probe refill failed for "
                      << zevryon::core::resource_class_name(resource_class) << '\n';
            return false;
        }
        ledger.release(resource_class, first_reservation);

        const ResourceSnapshot snapshot = ledger.snapshot(resource_class);
        if (snapshot.current_bytes != 0U || snapshot.accounting_errors != 0U ||
            snapshot.rejected_reservations != 0U) {
            std::cerr << "workload probe accounting drift for "
                      << zevryon::core::resource_class_name(resource_class) << '\n';
            return false;
        }
        checksum_text(checksum, zevryon::core::resource_class_name(resource_class));
        checksum_snapshot(checksum, snapshot);
    }

    if (!ledger.verify_rust_shadow() || !ledger.rust_shadow_healthy()) {
        std::cerr << "workload probe Rust verification failed after " << name << '\n';
        return false;
    }

    if (!first_workload) {
        workloads << ',';
    }
    first_workload = false;
    workloads << "{\"name\":\"" << name << "\",\"resource_classes\":" << Count
              << ",\"operations\":"
              << (ledger.rust_shadow_operations() - operations_before)
              << ",\"verifications\":"
              << (ledger.rust_shadow_verifications() - verifications_before)
              << ",\"mismatches\":" << ledger.rust_shadow_mismatches()
              << ",\"current_bytes\":" << ledger.total_current_bytes()
              << ",\"peak_bytes\":" << ledger.total_peak_bytes()
              << ",\"healthy\":"
              << (ledger.rust_shadow_healthy() ? "true" : "false") << '}';
    return true;
}

int run_workload_probe() {
    constexpr std::array<ResourceClass, 4> massivedoc{
        ResourceClass::SourceWindow,
        ResourceClass::CheckpointIndex,
        ResourceClass::DomProjection,
        ResourceClass::NetworkBuffer,
    };
    constexpr std::array<ResourceClass, 6> layout{
        ResourceClass::ComputedStyle,
        ResourceClass::LayoutFragment,
        ResourceClass::PaintCommand,
        ResourceClass::RasterTile,
        ResourceClass::ImageDecode,
        ResourceClass::CompositorSurface,
    };
    constexpr std::array<ResourceClass, 10> unicode{
        ResourceClass::UnicodeBuffer,
        ResourceClass::GraphemeCluster,
        ResourceClass::ScriptRun,
        ResourceClass::BidiRun,
        ResourceClass::BidiSequence,
        ResourceClass::BidiTypeResolution,
        ResourceClass::BidiNeutralResolution,
        ResourceClass::BidiImplicitLevel,
        ResourceClass::BidiVisualOrder,
        ResourceClass::BidiMirrorRequest,
    };
    constexpr std::array<ResourceClass, 14> font{
        ResourceClass::GlyphRun,
        ResourceClass::FontCatalog,
        ResourceClass::FontFallbackPlan,
        ResourceClass::FontDiscoverySnapshot,
        ResourceClass::FontResourceBytes,
        ResourceClass::FontResourceCacheMetadata,
        ResourceClass::FontResourceCacheRetention,
        ResourceClass::FontFileReadBuffer,
        ResourceClass::ShapingRunPlan,
        ResourceClass::MultiRunShapeMetadata,
        ResourceClass::GlyphClusterMap,
        ResourceClass::CaretBoundaryMap,
        ResourceClass::LineBreakOpportunityMap,
        ResourceClass::LineSelectionMap,
    };
    constexpr std::array<ResourceClass, 2> browser{
        ResourceClass::JavaScriptHeap,
        ResourceClass::AccessibilityProjection,
    };

    ResourceLedger ledger;
    if (!ledger.rust_shadow_enabled()) {
        std::cerr << "Rust shadow workload probe requires ZEVRYON_RUST_LEDGER_SHADOW=ON\n";
        return 3;
    }

    std::uint64_t checksum = 14'695'981'039'346'656'037ULL;
    std::ostringstream workloads;
    bool first_workload = true;
    if (!apply_workload_group(ledger, "massivedoc", massivedoc, checksum, workloads, first_workload) ||
        !apply_workload_group(ledger, "layout", layout, checksum, workloads, first_workload) ||
        !apply_workload_group(ledger, "unicode", unicode, checksum, workloads, first_workload) ||
        !apply_workload_group(ledger, "font", font, checksum, workloads, first_workload) ||
        !apply_workload_group(ledger, "browser", browser, checksum, workloads, first_workload)) {
        return 1;
    }

    const bool healthy = ledger.rust_shadow_healthy();
    const bool accounting_clean = ledger.accounting_clean();
    const bool within_hard_limits = ledger.within_hard_limits();
    const bool current_zero = ledger.total_current_bytes() == 0U;
    const bool mismatch_free = ledger.rust_shadow_mismatches() == 0U;
    if (!healthy || !accounting_clean || !within_hard_limits || !current_zero || !mismatch_free) {
        std::cerr << "Rust shadow workload probe final gate failed\n";
        return 1;
    }

    std::cout << "{\"schema\":\"zevryon.rust-shadow-workload-probe.v1\""
              << ",\"resource_class_count\":" << zevryon::core::resource_class_count
              << ",\"trace_checksum\":\"" << hex_u64(checksum) << "\""
              << ",\"rust_shadow_enabled\":true"
              << ",\"rust_shadow_healthy\":" << (healthy ? "true" : "false")
              << ",\"rust_shadow_operations\":" << ledger.rust_shadow_operations()
              << ",\"rust_shadow_verifications\":" << ledger.rust_shadow_verifications()
              << ",\"rust_shadow_mismatches\":" << ledger.rust_shadow_mismatches()
              << ",\"total_current_bytes\":" << ledger.total_current_bytes()
              << ",\"total_peak_bytes\":" << ledger.total_peak_bytes()
              << ",\"within_hard_limits\":" << (within_hard_limits ? "true" : "false")
              << ",\"accounting_clean\":" << (accounting_clean ? "true" : "false")
              << ",\"workloads\":[" << workloads.str() << ']'
              << ",\"shadow\":" << ledger.rust_shadow_json()
              << ",\"ledger\":" << ledger.json() << "}\n";
    return 0;
}

int run_contract_tests() {
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

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--emit-rust-shadow-workload-probe") {
        return run_workload_probe();
    }
    if (argc != 1) {
        std::cerr << "Usage: zevryon-resource-ledger-tests [--emit-rust-shadow-workload-probe]\n";
        return 2;
    }
    return run_contract_tests();
}
