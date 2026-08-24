#pragma once

#include "layout_checkpoint.hpp"
#include "layout_window.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace zevryon::massivedoc {

enum class ZenithMemoryPressure : std::uint8_t {
    Background = 0,
    Critical
};

enum class ZenithHotScrollLayoutStatus : std::uint8_t {
    Ready = 0,
    WouldBlockCheckpoint,
    WouldBlockSource,
    WouldBlockHeightPersistence
};

struct ZenithHotScrollStats {
    std::uint64_t layout_calls{0};
    std::uint64_t checkpoint_cache_hits{0};
    std::uint64_t checkpoint_cache_misses{0};
    std::uint64_t checkpoint_cache_evictions{0};
    std::uint64_t source_window_cache_hits{0};
    std::uint64_t source_window_cache_misses{0};
    std::uint64_t source_window_cache_evictions{0};
    std::uint64_t background_trim_calls{0};
    std::uint64_t critical_trim_calls{0};
    std::uint64_t trim_reclaimed_bytes{0};
    std::size_t checkpoint_cache_bytes{0};
    std::size_t checkpoint_cache_peak_bytes{0};
    std::size_t source_window_cache_bytes{0};
    std::size_t source_window_cache_peak_bytes{0};
    std::size_t source_scratch_capacity_bytes{0};
    std::size_t source_scratch_peak_bytes{0};
    std::size_t fragment_scratch_capacity_bytes{0};
    std::size_t fragment_scratch_peak_bytes{0};
};

class ZenithHotScrollSession {
public:
    explicit ZenithHotScrollSession(
        const std::filesystem::path& store_root,
        LayoutConfig config = {});
    ~ZenithHotScrollSession();

    ZenithHotScrollSession(const ZenithHotScrollSession&) = delete;
    ZenithHotScrollSession& operator=(const ZenithHotScrollSession&) = delete;

    bool open(std::string* error);
    bool move_logical_record(
        std::uint64_t from_index,
        std::uint64_t to_index,
        std::string* error);
    bool layout(
        std::uint64_t scroll_y_q8,
        std::uint32_t viewport_width_q8,
        std::uint64_t viewport_height_q8,
        std::uint64_t overscan_q8,
        std::size_t max_fragments,
        LayoutWindowResult* result,
        bool* used_checkpoint_path,
        std::string* error);

    // Execute the same layout algorithm with a strict resident-data-only
    // policy. Cache misses or required arena-height persistence are reported as
    // WouldBlock* outcomes instead of falling through to synchronous disk I/O.
    // A WouldBlock* outcome is a scheduling/readiness result, not corruption.
    bool layout_nonblocking(
        std::uint64_t scroll_y_q8,
        std::uint32_t viewport_width_q8,
        std::uint64_t viewport_height_q8,
        std::uint64_t overscan_q8,
        std::size_t max_fragments,
        LayoutWindowResult* result,
        bool* used_checkpoint_path,
        ZenithHotScrollLayoutStatus* status,
        std::string* error);

    // Admit an exact speculative source window under the same physical cache
    // key used by the synchronous hot-scroll path. Only complete exact-length
    // windows are accepted; partial/ambiguous payloads fail closed.
    bool admit_prefetched_source_window(
        std::uint64_t source_record_index,
        std::uint64_t source_offset,
        std::size_t request_bytes,
        std::vector<std::byte> bytes) noexcept;

    // Background pressure drops expensive source windows and transient working
    // buffers while preserving parsed checkpoints for a fast tab resume.
    // Critical pressure additionally releases parsed checkpoint state. Both
    // modes retain immutable store/arena handles and logical document state.
    void trim_memory(ZenithMemoryPressure pressure) noexcept;
    void clear_source_window_cache() noexcept;
    const ZenithHotScrollStats& stats() const noexcept;
    std::uint64_t total_height_q8() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string zenith_hot_scroll_stats_json(const ZenithHotScrollStats& stats);

} // namespace zevryon::massivedoc
