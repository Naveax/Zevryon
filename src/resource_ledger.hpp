#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
#include "zevryon_rust_ffi.h"
#endif

namespace zevryon::core {

enum class ResourceClass : std::uint8_t {
    SourceWindow = 0,
    CheckpointIndex,
    UnicodeBuffer,
    GraphemeCluster,
    ScriptRun,
    BidiRun,
    BidiSequence,
    BidiTypeResolution,
    BidiNeutralResolution,
    GlyphRun,
    ComputedStyle,
    LayoutFragment,
    PaintCommand,
    RasterTile,
    ImageDecode,
    JavaScriptHeap,
    AccessibilityProjection,
    NetworkBuffer,
    DomProjection,
    CompositorSurface,
    BidiImplicitLevel,
    BidiVisualOrder,
    BidiMirrorRequest,
    FontCatalog,
    FontFallbackPlan,
    FontDiscoverySnapshot,
    FontResourceBytes,
    FontResourceCacheMetadata,
    FontResourceCacheRetention,
    FontFileReadBuffer,
    ShapingRunPlan,
    MultiRunShapeMetadata,
    GlyphClusterMap,
    CaretBoundaryMap,
    LineBreakOpportunityMap,
    LineSelectionMap,
    Count
};

constexpr std::size_t resource_class_count =
    static_cast<std::size_t>(ResourceClass::Count);

const char* resource_class_name(ResourceClass resource_class) noexcept;

struct ResourceSnapshot {
    std::size_t hard_limit_bytes{std::numeric_limits<std::size_t>::max()};
    std::size_t current_bytes{0};
    std::size_t peak_bytes{0};
    std::uint64_t reservations{0};
    std::uint64_t releases{0};
    std::uint64_t rejected_reservations{0};
    std::uint64_t accounting_errors{0};
    std::uint64_t cache_hits{0};
    std::uint64_t cache_misses{0};
    std::uint64_t evictions{0};
    std::uint64_t physical_read_bytes{0};
    std::uint64_t physical_write_bytes{0};
};

class ResourceLedger {
public:
    ResourceLedger() noexcept;

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
    std::string json() const;

    bool rust_shadow_enabled() const noexcept;
    bool rust_shadow_healthy() const noexcept;
    bool verify_rust_shadow() noexcept;
    std::uint64_t rust_shadow_operations() const noexcept;
    std::uint64_t rust_shadow_verifications() const noexcept;
    std::uint64_t rust_shadow_mismatches() const noexcept;
    std::string rust_shadow_json() const;

private:
    static std::size_t index_of(ResourceClass resource_class) noexcept;
    static std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept;

#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    enum class RustShadowMismatchKind : std::uint8_t {
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

    enum class RustShadowSnapshotField : std::uint8_t {
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

    void rust_shadow_set_hard_limit(
        ResourceClass resource_class,
        std::size_t bytes) noexcept;
    void rust_shadow_try_reserve(
        ResourceClass resource_class,
        std::size_t bytes,
        bool primary_result) noexcept;
    void rust_shadow_release(ResourceClass resource_class, std::size_t bytes) noexcept;
    void rust_shadow_record_cache_hit(ResourceClass resource_class) noexcept;
    void rust_shadow_record_cache_miss(ResourceClass resource_class) noexcept;
    void rust_shadow_record_eviction(ResourceClass resource_class) noexcept;
    void rust_shadow_record_physical_read(
        ResourceClass resource_class,
        std::uint64_t bytes) noexcept;
    void rust_shadow_record_physical_write(
        ResourceClass resource_class,
        std::uint64_t bytes) noexcept;
    void rust_shadow_complete_operation(ResourceClass resource_class) noexcept;
    void rust_shadow_record_mismatch(
        RustShadowMismatchKind kind,
        ResourceClass resource_class,
        RustShadowSnapshotField field,
        std::uint64_t expected,
        std::uint64_t actual) noexcept;
    bool rust_shadow_compare_snapshot(
        ResourceClass resource_class,
        const ResourceSnapshot& primary,
        const ZrResourceSnapshot& shadow) noexcept;
    static void increment_saturating(std::uint64_t& value) noexcept;

    ZrLedgerStorage rust_shadow_storage_{};
    std::uint64_t rust_shadow_operations_{0};
    std::uint64_t rust_shadow_verifications_{0};
    std::uint64_t rust_shadow_mismatches_{0};
    std::uint64_t rust_shadow_expected_{0};
    std::uint64_t rust_shadow_actual_{0};
    RustShadowMismatchKind rust_shadow_first_mismatch_{RustShadowMismatchKind::None};
    RustShadowSnapshotField rust_shadow_first_field_{RustShadowSnapshotField::None};
    ResourceClass rust_shadow_first_resource_class_{ResourceClass::SourceWindow};
    bool rust_shadow_initialized_{false};
#endif

    std::array<ResourceSnapshot, resource_class_count> resources_{};
    std::size_t total_current_bytes_{0};
    std::size_t total_peak_bytes_{0};
};

#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
static_assert(sizeof(ZrLedgerStorage) == ZR_LEDGER_STORAGE_BYTES);
static_assert(alignof(ZrLedgerStorage) == ZR_LEDGER_STORAGE_ALIGN);
static_assert(resource_class_count == ZR_RESOURCE_CLASS_COUNT);
#endif

} // namespace zevryon::core
