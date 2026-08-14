#include "massivedoc_cold_window.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

int main() {
    using namespace zevryon::massivedoc;

    std::string error;
    if (validate_cold_mapped_window_bytes(
            kMaximumColdMappedWindowBytes + 1U,
            &error)) {
        std::cerr << "oversized cold mapped window was accepted\n";
        return 1;
    }

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("zevryon-cold-window-" + std::to_string(unique));
    const std::filesystem::path file = root / "segment.bin";
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if (filesystem_error) {
        std::cerr << "cannot create cold-window test root\n";
        return 1;
    }

    constexpr std::size_t kFileBytes = 1024U * 1024U;
    constexpr std::size_t kWindowBytes = 256U * 1024U;
    constexpr std::size_t kTouchBytes = 128U * 1024U;
    std::vector<std::byte> payload(kFileBytes);
    for (std::size_t index = 0U; index < payload.size(); ++index) {
        payload[index] = static_cast<std::byte>(
            static_cast<unsigned char>((index * 17U + 3U) & 0xffU));
    }
    {
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        stream.write(
            reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
        if (!stream) {
            std::cerr << "cannot write cold-window fixture\n";
            return 1;
        }
    }

    ColdMappedWindow window(kWindowBytes);
    if (!window.touch(file, 0U, kTouchBytes, &error)) {
        std::cerr << "initial cold touch failed: " << error << '\n';
        return 1;
    }
    if (!window.touch(file, 64U * 1024U, 32U * 1024U, &error)) {
        std::cerr << "same-window cold touch failed: " << error << '\n';
        return 1;
    }
    if (!window.touch(file, 512U * 1024U, kTouchBytes, &error)) {
        std::cerr << "cold remap touch failed: " << error << '\n';
        return 1;
    }

    const auto stats = window.stats();
    if (stats.mapped_bytes != kWindowBytes ||
        stats.peak_mapped_bytes != kWindowBytes ||
        stats.mapping_hits != 1U ||
        stats.mapping_misses != 2U ||
        stats.remaps != 2U ||
        stats.touches != 3U ||
        stats.touched_bytes != (kTouchBytes + 32U * 1024U + kTouchBytes) ||
        !stats.ledger_within_hard_limits ||
        !stats.ledger_accounting_clean) {
        std::cerr << "cold mapped window stats mismatch\n";
        return 1;
    }

    if (window.touch(file, kFileBytes - 32U * 1024U, 64U * 1024U, &error)) {
        std::cerr << "out-of-range cold touch was accepted\n";
        return 1;
    }

    window.release();
    const auto released = window.stats();
    if (released.mapped_bytes != 0U ||
        released.peak_mapped_bytes != kWindowBytes ||
        !released.ledger_within_hard_limits ||
        !released.ledger_accounting_clean) {
        std::cerr << "cold mapped window release accounting mismatch\n";
        return 1;
    }

    std::filesystem::remove_all(root, filesystem_error);
    std::cout << "Zevryon MassiveDoc cold mapped window tests passed\n";
    return 0;
}
