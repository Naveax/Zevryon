#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace zevryon::massivedoc {

struct LayoutConfig {
    std::uint32_t average_advance_q8{8U * 256U};
    std::uint32_t line_height_q8{18U * 256U};
    std::uint32_t horizontal_padding_q8{12U * 256U};
    std::uint32_t vertical_padding_q8{12U * 256U};
    std::uint32_t width_bucket_q8{8U * 256U};
    std::size_t max_cache_bytes{8U * 1000U * 1000U};
};

struct LayoutFragment {
    std::uint64_t record_index{0};
    std::uint64_t logical_id{0};
    std::uint64_t source_start{0};
    std::uint64_t source_end{0};
    std::uint64_t x_q8{0};
    std::uint64_t y_q8{0};
    std::uint32_t width_q8{0};
    std::uint32_t height_q8{0};
    bool hard_break{false};
};

struct LayoutWindowResult {
    std::uint64_t query_start_q8{0};
    std::uint64_t query_end_q8{0};
    std::uint64_t total_height_q8{0};
    std::uint64_t source_bytes_read{0};
    std::uint64_t measured_records{0};
    std::uint64_t cache_hits{0};
    std::uint64_t cache_misses{0};
    std::size_t cache_bytes{0};
    bool truncated{false};
    bool height_saturated{false};
    std::vector<LayoutFragment> fragments;
};

class LayoutWindowEngine {
public:
    explicit LayoutWindowEngine(const std::filesystem::path& store_root, LayoutConfig config = {});
    ~LayoutWindowEngine();

    LayoutWindowEngine(const LayoutWindowEngine&) = delete;
    LayoutWindowEngine& operator=(const LayoutWindowEngine&) = delete;

    bool open(std::string* error);
    bool layout(
        std::uint64_t scroll_y_q8,
        std::uint32_t viewport_width_q8,
        std::uint64_t viewport_height_q8,
        std::uint64_t overscan_q8,
        std::size_t max_fragments,
        LayoutWindowResult* result,
        std::string* error);
    std::size_t cache_bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string layout_window_json(const LayoutWindowResult& result);

} // namespace zevryon::massivedoc
