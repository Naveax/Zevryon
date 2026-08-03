#include "resource_ledger.hpp"
#include "rust_resource_ledger.hpp"

#include <cstddef>
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

bool snapshots_equal(
    const zevryon::core::ResourceSnapshot& left,
    const zevryon::core::ResourceSnapshot& right) {
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

} // namespace

int main() {
    using zevryon::core::ResourceClass;
    using zevryon::core::ResourceLedger;
    using zevryon::core::ResourceSnapshot;
    using zevryon::core::RustResourceLedger;
    using zevryon::core::resource_class_count;

    ResourceLedger cpp;
    RustResourceLedger rust;

    if (!require(rust.valid(), "Rust ledger initializes") ||
        !require(
            RustResourceLedger::abi_version() == ZR_ABI_VERSION,
            "Rust ABI version matches C header") ||
        !require(
            RustResourceLedger::ffi_resource_class_count() == resource_class_count,
            "Rust and C++ resource-class counts match") ||
        !require(
            zr_ledger_storage_size() == sizeof(ZrLedgerStorage),
            "Rust reports the fixed storage size") ||
        !require(
            zr_ledger_storage_alignment() == alignof(ZrLedgerStorage),
            "Rust reports the fixed storage alignment")) {
        return 1;
    }

    const auto set_limit = [&](ResourceClass resource_class, std::size_t bytes) {
        cpp.set_hard_limit(resource_class, bytes);
        return rust.set_hard_limit(resource_class, bytes);
    };
    const auto reserve = [&](ResourceClass resource_class, std::size_t bytes) {
        const bool cpp_result = cpp.try_reserve(resource_class, bytes);
        const bool rust_result = rust.try_reserve(resource_class, bytes);
        return cpp_result == rust_result;
    };
    const auto release = [&](ResourceClass resource_class, std::size_t bytes) {
        cpp.release(resource_class, bytes);
        return rust.release(resource_class, bytes);
    };

    if (!require(set_limit(ResourceClass::SourceWindow, 100U), "set source limit") ||
        !require(set_limit(ResourceClass::CheckpointIndex, 40U), "set checkpoint limit") ||
        !require(reserve(ResourceClass::SourceWindow, 60U), "reserve 60 equally") ||
        !require(reserve(ResourceClass::SourceWindow, 50U), "reject 50 equally") ||
        !require(reserve(ResourceClass::SourceWindow, 40U), "reserve final 40 equally")) {
        return 1;
    }

    cpp.record_cache_hit(ResourceClass::SourceWindow);
    cpp.record_cache_miss(ResourceClass::SourceWindow);
    cpp.record_eviction(ResourceClass::SourceWindow);
    cpp.record_physical_read(ResourceClass::SourceWindow, 65'536U);
    cpp.record_physical_write(ResourceClass::SourceWindow, 4'096U);
    if (!require(rust.record_cache_hit(ResourceClass::SourceWindow), "record cache hit") ||
        !require(rust.record_cache_miss(ResourceClass::SourceWindow), "record cache miss") ||
        !require(rust.record_eviction(ResourceClass::SourceWindow), "record eviction") ||
        !require(
            rust.record_physical_read(ResourceClass::SourceWindow, 65'536U),
            "record physical read") ||
        !require(
            rust.record_physical_write(ResourceClass::SourceWindow, 4'096U),
            "record physical write")) {
        return 1;
    }

    if (!require(release(ResourceClass::SourceWindow, 40U), "release 40") ||
        !require(release(ResourceClass::SourceWindow, 60U), "release 60") ||
        !require(reserve(ResourceClass::CheckpointIndex, 40U), "reserve checkpoint cap") ||
        !require(reserve(ResourceClass::CheckpointIndex, 1U), "reject checkpoint overflow") ||
        !require(release(ResourceClass::CheckpointIndex, 40U), "release checkpoint")) {
        return 1;
    }

    cpp.record_physical_read(
        ResourceClass::NetworkBuffer,
        std::numeric_limits<std::uint64_t>::max() - 2U);
    cpp.record_physical_read(ResourceClass::NetworkBuffer, 8U);
    if (!require(
            rust.record_physical_read(
                ResourceClass::NetworkBuffer,
                std::numeric_limits<std::uint64_t>::max() - 2U),
            "record saturating physical read prefix") ||
        !require(
            rust.record_physical_read(ResourceClass::NetworkBuffer, 8U),
            "record saturating physical read suffix")) {
        return 1;
    }

    if (!require(set_limit(ResourceClass::LayoutFragment, 32U), "set error fixture limit") ||
        !require(reserve(ResourceClass::LayoutFragment, 16U), "reserve error fixture") ||
        !require(release(ResourceClass::LayoutFragment, 17U), "over-release fails closed")) {
        return 1;
    }

    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        const auto resource_class = static_cast<ResourceClass>(index);
        const ResourceSnapshot cpp_snapshot = cpp.snapshot(resource_class);
        ResourceSnapshot rust_snapshot{};
        if (!require(
                rust.snapshot(resource_class, rust_snapshot),
                "Rust snapshot available for class " + std::to_string(index)) ||
            !require(
                snapshots_equal(cpp_snapshot, rust_snapshot),
                "C++ and Rust snapshots match for class " + std::to_string(index))) {
            return 1;
        }
    }

    if (!require(
            cpp.total_current_bytes() == rust.total_current_bytes(),
            "total current bytes match") ||
        !require(
            cpp.total_peak_bytes() == rust.total_peak_bytes(),
            "total peak bytes match") ||
        !require(
            cpp.within_hard_limits() == rust.within_hard_limits(),
            "hard-limit status matches") ||
        !require(
            cpp.accounting_clean() == rust.accounting_clean(),
            "accounting status matches")) {
        return 1;
    }

    std::cout << "Rust/C++ resource ledger equivalence passed\n";
    return 0;
}
