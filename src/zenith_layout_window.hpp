#pragma once

#include "layout_window.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace zevryon::massivedoc {

bool layout_window_with_persistent_checkpoints(
    const std::filesystem::path& store_root,
    LayoutConfig config,
    std::uint64_t scroll_y_q8,
    std::uint32_t viewport_width_q8,
    std::uint64_t viewport_height_q8,
    std::uint64_t overscan_q8,
    std::size_t max_fragments,
    LayoutWindowResult* result,
    bool* used_checkpoint_path,
    std::string* error);

std::string zenith_layout_window_json(const LayoutWindowResult& result);

} // namespace zevryon::massivedoc
