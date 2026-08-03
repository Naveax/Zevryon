#include "shadow_resource_ledger.hpp"

#include <limits>

namespace zevryon::core {

ShadowResourceLedger::ShadowResourceLedger(
    std::uint64_t verification_interval) noexcept
    : verification_interval_(verification_interval) {
    if (!shadow_.valid()) {
        record_mismatch(
            ShadowMismatchKind::RustUnavailable,
            ResourceClass::SourceWindow,
            ShadowSnapshotField::None,
            1U,
            0U);
    }
    if (RustResourceLedger::abi_version() != ZR_ABI_VERSION) {
        record_mismatch(
            ShadowMismatchKind::AbiVersion,
            ResourceClass::SourceWindow,
            ShadowSnapshotField::None,
            ZR_ABI_VERSION,
            RustResourceLedger::abi_version());
    }
    if (RustResourceLedger::ffi_resource_class_count() != resource_class_count) {
        record_mismatch(
            ShadowMismatchKind::ResourceClassCount,
            ResourceClass::SourceWindow,
            ShadowSnapshotField::None,
            static_cast<std::uint64_t>(resource_class_count),
            RustResourceLedger::ffi_resource_class_count());
    }
}

void ShadowResourceLedger::set_hard_limit(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    primary_.set_hard_limit(resource_class, bytes);
    if (!shadow_.set_hard_limit(resource_class, bytes)) {
        record_mismatch(
            ShadowMismatchKind::OperationResult,
            resource_class,
            ShadowSnapshotField::HardLimitBytes,
            1U,
            0U);
    }
    complete_operation(resource_class);
}

bool ShadowResourceLedger::try_reserve(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    const bool primary_result = primary_.try_reserve(resource_class, bytes);
    const bool shadow_result = shadow_.try_reserve(resource_class, bytes);
    if (primary_result != shadow_result) {
        record_mismatch(
            ShadowMismatchKind::OperationResult,
            resource_class,
            ShadowSnapshotField::Reservations,
            primary_result ? 1U : 0U,
            shadow_result ? 1U : 0U);
    }
    complete_operation(resource_class);
    return primary_result;
}

void ShadowResourceLedger::release(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    primary_.release(resource_class, bytes);
    if (!shadow_.release(resource_class, bytes)) {
        record_mismatch(
            ShadowMismatchKind::OperationResult,
            resource_class,
            ShadowSnapshotField::Releases,
            1U,
            0U);
    }
    complete_operation(resource_class);
}

void ShadowResourceLedger::record_cache_hit(ResourceClass resource_class) noexcept {
    primary_.record_cache_hit(resource_class);
    if (!shadow_.record_cache_hit(resource_class)) {
        record_mismatch(
            ShadowMismatchKind::OperationResult,
            resource_class,
            ShadowSnapshotField::CacheHits,
            1U,
            0U);
    }
    complete_operation(resource_class);
}

void ShadowResourceLedger::record_cache_miss(ResourceClass resource_class) noexcept {
    primary_.record_cache_miss(resource_class);
    if (!shadow_.record_cache_miss(resource_class)) {
        record_mismatch(
            ShadowMismatchKind::OperationResult,
            resource_class,
            ShadowSnapshotField::CacheMisses,
            1U,
            0U);
    }
    complete_operation(resource_class);
}

void ShadowResourceLedger::record_eviction(ResourceClass resource_class) noexcept {
    primary_.record_eviction(resource_class);
    if (!shadow_.record_eviction(resource_class)) {
        record_mismatch(
            ShadowMismatchKind::OperationResult,
            resource_class,
            ShadowSnapshotField::Evictions,
            1U,
            0U);
    }
    complete_operation(resource_class);
}

void ShadowResourceLedger::record_physical_read(
    ResourceClass resource_class,
    std::uint64_t bytes) noexcept {
    primary_.record_physical_read(resource_class, bytes);
    if (!shadow_.record_physical_read(resource_class, bytes)) {
        record_mismatch(
            ShadowMismatchKind::OperationResult,
            resource_class,
            ShadowSnapshotField::PhysicalReadBytes,
            1U,
            0U);
    }
    complete_operation(resource_class);
}

void ShadowResourceLedger::record_physical_write(
    ResourceClass resource_class,
    std::uint64_t bytes) noexcept {
    primary_.record_physical_write(resource_class, bytes);
    if (!shadow_.record_physical_write(resource_class, bytes)) {
        record_mismatch(
            ShadowMismatchKind::OperationResult,
            resource_class,
            ShadowSnapshotField::PhysicalWriteBytes,
            1U,
            0U);
    }
    complete_operation(resource_class);
}

ResourceSnapshot ShadowResourceLedger::snapshot(
    ResourceClass resource_class) const noexcept {
    return primary_.snapshot(resource_class);
}

std::size_t ShadowResourceLedger::total_current_bytes() const noexcept {
    return primary_.total_current_bytes();
}

std::size_t ShadowResourceLedger::total_peak_bytes() const noexcept {
    return primary_.total_peak_bytes();
}

bool ShadowResourceLedger::within_hard_limits() const noexcept {
    return primary_.within_hard_limits();
}

bool ShadowResourceLedger::accounting_clean() const noexcept {
    return primary_.accounting_clean();
}

bool ShadowResourceLedger::verify_now() noexcept {
    increment_saturating(diagnostics_.verifications);
    bool matches = true;

    if (!shadow_.valid()) {
        record_mismatch(
            ShadowMismatchKind::RustUnavailable,
            ResourceClass::SourceWindow,
            ShadowSnapshotField::None,
            1U,
            0U);
        return false;
    }

    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        const auto resource_class = static_cast<ResourceClass>(index);
        const ResourceSnapshot primary_snapshot = primary_.snapshot(resource_class);
        ResourceSnapshot shadow_snapshot{};
        if (!shadow_.snapshot(resource_class, shadow_snapshot)) {
            record_mismatch(
                ShadowMismatchKind::SnapshotUnavailable,
                resource_class,
                ShadowSnapshotField::None,
                1U,
                0U);
            matches = false;
            continue;
        }
        matches = compare_snapshot(resource_class, primary_snapshot, shadow_snapshot) && matches;
    }

    const auto compare_aggregate = [this, &matches](
                                       ShadowMismatchKind kind,
                                       std::uint64_t expected,
                                       std::uint64_t actual) {
        if (expected != actual) {
            record_mismatch(
                kind,
                ResourceClass::SourceWindow,
                ShadowSnapshotField::None,
                expected,
                actual);
            matches = false;
        }
    };

    compare_aggregate(
        ShadowMismatchKind::TotalCurrentBytes,
        static_cast<std::uint64_t>(primary_.total_current_bytes()),
        static_cast<std::uint64_t>(shadow_.total_current_bytes()));
    compare_aggregate(
        ShadowMismatchKind::TotalPeakBytes,
        static_cast<std::uint64_t>(primary_.total_peak_bytes()),
        static_cast<std::uint64_t>(shadow_.total_peak_bytes()));
    compare_aggregate(
        ShadowMismatchKind::WithinHardLimits,
        primary_.within_hard_limits() ? 1U : 0U,
        shadow_.within_hard_limits() ? 1U : 0U);
    compare_aggregate(
        ShadowMismatchKind::AccountingClean,
        primary_.accounting_clean() ? 1U : 0U,
        shadow_.accounting_clean() ? 1U : 0U);

    return matches;
}

bool ShadowResourceLedger::healthy() const noexcept {
    return diagnostics_.mismatches == 0U;
}

const ShadowLedgerDiagnostics& ShadowResourceLedger::diagnostics() const noexcept {
    return diagnostics_;
}

std::uint64_t ShadowResourceLedger::verification_interval() const noexcept {
    return verification_interval_;
}

#if defined(ZEVRYON_SHADOW_LEDGER_TEST_HOOKS)
bool ShadowResourceLedger::inject_shadow_reservation_for_testing(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    return shadow_.try_reserve(resource_class, bytes);
}
#endif

void ShadowResourceLedger::complete_operation(ResourceClass resource_class) noexcept {
    static_cast<void>(resource_class);
    increment_saturating(diagnostics_.operations);
    if (verification_interval_ != 0U &&
        diagnostics_.operations % verification_interval_ == 0U) {
        static_cast<void>(verify_now());
    }
}

void ShadowResourceLedger::record_mismatch(
    ShadowMismatchKind kind,
    ResourceClass resource_class,
    ShadowSnapshotField field,
    std::uint64_t expected,
    std::uint64_t actual) noexcept {
    increment_saturating(diagnostics_.mismatches);
    if (diagnostics_.first_mismatch != ShadowMismatchKind::None) {
        return;
    }
    diagnostics_.first_mismatch = kind;
    diagnostics_.first_resource_class = resource_class;
    diagnostics_.first_field = field;
    diagnostics_.expected = expected;
    diagnostics_.actual = actual;
}

bool ShadowResourceLedger::compare_snapshot(
    ResourceClass resource_class,
    const ResourceSnapshot& primary,
    const ResourceSnapshot& shadow) noexcept {
    bool matches = true;
    const auto compare = [this, resource_class, &matches](
                             ShadowSnapshotField field,
                             std::uint64_t expected,
                             std::uint64_t actual) {
        if (expected != actual) {
            record_mismatch(
                ShadowMismatchKind::SnapshotField,
                resource_class,
                field,
                expected,
                actual);
            matches = false;
        }
    };

    compare(
        ShadowSnapshotField::HardLimitBytes,
        static_cast<std::uint64_t>(primary.hard_limit_bytes),
        static_cast<std::uint64_t>(shadow.hard_limit_bytes));
    compare(
        ShadowSnapshotField::CurrentBytes,
        static_cast<std::uint64_t>(primary.current_bytes),
        static_cast<std::uint64_t>(shadow.current_bytes));
    compare(
        ShadowSnapshotField::PeakBytes,
        static_cast<std::uint64_t>(primary.peak_bytes),
        static_cast<std::uint64_t>(shadow.peak_bytes));
    compare(ShadowSnapshotField::Reservations, primary.reservations, shadow.reservations);
    compare(ShadowSnapshotField::Releases, primary.releases, shadow.releases);
    compare(
        ShadowSnapshotField::RejectedReservations,
        primary.rejected_reservations,
        shadow.rejected_reservations);
    compare(
        ShadowSnapshotField::AccountingErrors,
        primary.accounting_errors,
        shadow.accounting_errors);
    compare(ShadowSnapshotField::CacheHits, primary.cache_hits, shadow.cache_hits);
    compare(ShadowSnapshotField::CacheMisses, primary.cache_misses, shadow.cache_misses);
    compare(ShadowSnapshotField::Evictions, primary.evictions, shadow.evictions);
    compare(
        ShadowSnapshotField::PhysicalReadBytes,
        primary.physical_read_bytes,
        shadow.physical_read_bytes);
    compare(
        ShadowSnapshotField::PhysicalWriteBytes,
        primary.physical_write_bytes,
        shadow.physical_write_bytes);
    return matches;
}

void ShadowResourceLedger::increment_saturating(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

} // namespace zevryon::core
