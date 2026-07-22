#pragma once

#include "layout_window.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

struct ZenithHotScrollStats {
    std::uint64_t layout_calls{0};
    std::uint64_t checkpoint_cache_hits{0};
    std::uint64_t checkpoint_cache_misses{0};
    std::uint64_t checkpoint_cache_evictions{0};
    std::uint64_t source_window_cache_hits{0};
    std::uint64_t source_window_cache_misses{0};
    std::uint64_t source_window_cache_evictions{0};
    std::size_t checkpoint_cache_bytes{0};
    std::size_t checkpoint_cache_peak_bytes{0};
    std::size_t source_window_cache_bytes{0};
    std::size_t source_window_cache_peak_bytes{0};
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
    bool layout(
        std::uint64_t scroll_y_q8,
        std::uint32_t viewport_width_q8,
        std::uint64_t viewport_height_q8,
        std::uint64_t overscan_q8,
        std::size_t max_fragments,
        LayoutWindowResult* result,
        bool* used_checkpoint_path,
        std::string* error);

    void clear_source_window_cache() noexcept;
    const ZenithHotScrollStats& stats() const noexcept;
    std::uint64_t total_height_q8() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string zenith_hot_scroll_stats_json(const ZenithHotScrollStats& stats);

} // namespace zevryon::massivedoc
