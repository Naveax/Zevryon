#pragma once

#include "resource_ledger.hpp"
#include "rust_resource_ledger.hpp"

#include <cstddef>
#include <cstdint>

namespace zevryon::core {

enum class ShadowMismatchKind : std::uint8_t {
    None = 0,
    RustUnavailable,
    AbiVersion,
    ResourceClassCount,
    OperationResult,
    SnapshotUnavailable,
    SnapshotField,
    TotalCurrentBytes,
    TotalPeakBytes,
    WithinHardLimits,
    AccountingClean,
};

enum class ShadowSnapshotField : std::uint8_t {
    None = 0,
    HardLimitBytes,
    CurrentBytes,
    PeakBytes,
    Reservations,
    Releases,
    RejectedReservations,
    AccountingErrors,
    CacheHits,
    CacheMisses,
    Evictions,
    PhysicalReadBytes,
    PhysicalWriteBytes,
};

struct ShadowLedgerDiagnostics {
    std::uint64_t operations{0};
    std::uint64_t verifications{0};
    std::uint64_t mismatches{0};
    ShadowMismatchKind first_mismatch{ShadowMismatchKind::None};
    ShadowSnapshotField first_field{ShadowSnapshotField::None};
    ResourceClass first_resource_class{ResourceClass::SourceWindow};
    std::uint64_t expected{0};
    std::uint64_t actual{0};
};

class ShadowResourceLedger final {
public:
    explicit ShadowResourceLedger(
        std::uint64_t verification_interval = 1'024U) noexcept;

    ShadowResourceLedger(const ShadowResourceLedger&) = delete;
    ShadowResourceLedger& operator=(const ShadowResourceLedger&) = delete;
    ShadowResourceLedger(ShadowResourceLedger&&) = delete;
    ShadowResourceLedger& operator=(ShadowResourceLedger&&) = delete;

    void set_hard_limit(ResourceClass resource_class, std::size_t bytes) noexcept;
    bool try_reserve(ResourceClass resource_class, std::size_t bytes) noexcept;
    void release(ResourceClass resource_class, std::size_t bytes) noexcept;

    void record_cache_hit(ResourceClass resource_class) noexcept;
    void record_cache_miss(ResourceClass resource_class) noexcept;
    void record_eviction(ResourceClass resource_class) noexcept;
    void record_physical_read(ResourceClass resource_class, std::uint64_t bytes) noexcept;
    void record_physical_write(ResourceClass resource_class, std::uint64_t bytes) noexcept;

    ResourceSnapshot snapshot(ResourceClass resource_class) const noexcept;
    std::size_t total_current_bytes() const noexcept;
    std::size_t total_peak_bytes() const noexcept;
    bool within_hard_limits() const noexcept;
    bool accounting_clean() const noexcept;

    bool verify_now() noexcept;
    bool healthy() const noexcept;
    const ShadowLedgerDiagnostics& diagnostics() const noexcept;
    std::uint64_t verification_interval() const noexcept;

#if defined(ZEVRYON_SHADOW_LEDGER_TEST_HOOKS)
    bool inject_shadow_reservation_for_testing(
        ResourceClass resource_class,
        std::size_t bytes) noexcept;
#endif

private:
    void complete_operation(ResourceClass resource_class) noexcept;
    void record_mismatch(
        ShadowMismatchKind kind,
        ResourceClass resource_class,
        ShadowSnapshotField field,
        std::uint64_t expected,
        std::uint64_t actual) noexcept;
    bool compare_snapshot(
        ResourceClass resource_class,
        const ResourceSnapshot& primary,
        const ResourceSnapshot& shadow) noexcept;
    static void increment_saturating(std::uint64_t& value) noexcept;

    ResourceLedger primary_{};
    RustResourceLedger shadow_{};
    std::uint64_t verification_interval_{0};
    ShadowLedgerDiagnostics diagnostics_{};
};

} // namespace zevryon::core
