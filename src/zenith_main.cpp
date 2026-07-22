#include "zenith_layout_window.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>

namespace {

template <typename T>
std::optional<T> parse_number(std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint64_t> pixels_to_q8(std::string_view text) {
    const auto pixels = parse_number<std::uint64_t>(text);
    if (!pixels || *pixels > std::numeric_limits<std::uint64_t>::max() / 256U) {
        return std::nullopt;
    }
    return *pixels * 256U;
}

void usage() {
    std::cerr
        << "Usage: zevryon-zenith-layout <store-dir> <scroll-y-px> <width-px>"
           " <height-px> [overscan-px] [max-fragments] [stride-kib]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc > 8) {
        usage();
        return 2;
    }
    const auto scroll_y = pixels_to_q8(argv[2]);
    const auto width = pixels_to_q8(argv[3]);
    const auto height = pixels_to_q8(argv[4]);
    const auto overscan =
        argc >= 6 ? pixels_to_q8(argv[5])
                  : std::optional<std::uint64_t>{720U * 256U};
    const auto max_fragments =
        argc >= 7 ? parse_number<std::size_t>(argv[6])
                  : std::optional<std::size_t>{512U};
    const auto stride_kib =
        argc == 8 ? parse_number<std::uint32_t>(argv[7])
                  : std::optional<std::uint32_t>{64U};
    if (!scroll_y || !width || *width == 0U ||
        *width > std::numeric_limits<std::uint32_t>::max() || !height ||
        *height == 0U || !overscan || !max_fragments || *max_fragments == 0U ||
        !stride_kib || *stride_kib == 0U ||
        *stride_kib > std::numeric_limits<std::uint32_t>::max() / 1024U) {
        std::cerr << "invalid ZENITH layout arguments\n";
        return 2;
    }

    zevryon::massivedoc::LayoutConfig config;
    config.checkpoint_stride_bytes = *stride_kib * 1024U;
    zevryon::massivedoc::LayoutWindowResult result;
    bool used_checkpoint_path = false;
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    if (!zevryon::massivedoc::layout_window_with_persistent_checkpoints(
            argv[1],
            config,
            *scroll_y,
            static_cast<std::uint32_t>(*width),
            *height,
            *overscan,
            *max_fragments,
            &result,
            &used_checkpoint_path,
            &error)) {
        std::cerr << "ZENITH layout failed: " << error << '\n';
        return 1;
    }
    if (!used_checkpoint_path) {
        std::cerr << "ZENITH checkpoint path unavailable; use legacy layout-window fallback\n";
        return 3;
    }
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "{\"operation\":\"zenith-layout-window\",\"seconds\":" << elapsed
              << ",\"layout\":"
              << zevryon::massivedoc::zenith_layout_window_json(result) << "}\n";
    return 0;
}
