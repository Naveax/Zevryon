#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

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

private:
    static std::size_t index_of(ResourceClass resource_class) noexcept;
    static std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept;

    std::array<ResourceSnapshot, resource_class_count> resources_{};
    std::size_t total_current_bytes_{0};
    std::size_t total_peak_bytes_{0};
};

} // namespace zevryon::core
