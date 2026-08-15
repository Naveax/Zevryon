#include "massivedoc_exact_match.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <span>
#include <string_view>
#include <vector>

namespace {
using namespace zevryon::massivedoc;

[[noreturn]] void die(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}
void require(bool value, std::string_view message) { if (!value) die(message); }

std::size_t scalar_reference(
    std::span<const std::byte> haystack,
    std::span<const std::byte> needle) {
    if (needle.empty()) return 0U;
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end());
    return it == haystack.end()
        ? kExactByteMatchNotFound
        : static_cast<std::size_t>(std::distance(haystack.begin(), it));
}

void test_edges() {
    const std::vector<std::byte> empty;
    const std::vector<std::byte> one{std::byte{0x00}};
    require(find_exact_bytes(empty, empty) == 0U, "empty/empty");
    require(find_exact_bytes(one, empty) == 0U, "empty needle");
    require(find_exact_bytes(empty, one) == kExactByteMatchNotFound, "needle larger than haystack");
    require(find_exact_bytes(one, one) == 0U, "single-byte exact");

    const std::vector<std::byte> repeated{
        std::byte{0x61}, std::byte{0x61}, std::byte{0x61}, std::byte{0x62}};
    const std::vector<std::byte> needle{
        std::byte{0x61}, std::byte{0x61}, std::byte{0x62}};
    require(find_exact_bytes(repeated, needle) == 1U, "first-match overlap semantics");
}

void test_binary_and_batch_boundaries() {
    std::vector<std::byte> haystack(80U, std::byte{0x55});
    const std::vector<std::byte> needle{
        std::byte{0x00}, std::byte{0xff}, std::byte{0x7f}, std::byte{0x00}};
    const std::size_t locations[] = {0U, 15U, 16U, 31U, 47U, 76U};
    for (const std::size_t location : locations) {
        std::fill(haystack.begin(), haystack.end(), std::byte{0x55});
        std::copy(
            needle.begin(), needle.end(),
            haystack.begin() + static_cast<std::ptrdiff_t>(location));
        ExactByteMatchStats stats;
        require(find_exact_bytes(haystack, needle, &stats) == location, "binary boundary match");
        if (exact_byte_match_simd_available() && location >= 16U) {
            require(stats.simd_batches != 0U, "SIMD-capable build did not execute SIMD batch");
        }
    }
}

void test_randomized_scalar_equivalence() {
    std::mt19937_64 random(0x5a657672796f6eULL);
    for (std::size_t haystack_size = 0U; haystack_size <= 160U; ++haystack_size) {
        for (std::size_t needle_size = 0U; needle_size <= 40U; ++needle_size) {
            for (std::size_t trial = 0U; trial < 12U; ++trial) {
                std::vector<std::byte> haystack(haystack_size);
                std::vector<std::byte> needle(needle_size);
                for (auto& value : haystack) {
                    value = std::byte{static_cast<unsigned char>(random() & 0xffU)};
                }
                for (auto& value : needle) {
                    value = std::byte{static_cast<unsigned char>(random() & 0xffU)};
                }
                if (!needle.empty() && needle.size() <= haystack.size() && (random() & 1U) != 0U) {
                    const std::size_t position = static_cast<std::size_t>(
                        random() % static_cast<std::uint64_t>(haystack.size() - needle.size() + 1U));
                    std::copy(
                        needle.begin(), needle.end(),
                        haystack.begin() + static_cast<std::ptrdiff_t>(position));
                }
                ExactByteMatchStats stats;
                const std::size_t actual = find_exact_bytes(haystack, needle, &stats);
                const std::size_t expected = scalar_reference(haystack, needle);
                if (actual != expected) {
                    std::cerr << "equivalence mismatch haystack=" << haystack_size
                              << " needle=" << needle_size
                              << " trial=" << trial
                              << " actual=" << actual
                              << " expected=" << expected << '\n';
                    std::exit(1);
                }
            }
        }
    }
}

} // namespace

int main() {
    test_edges();
    test_binary_and_batch_boundaries();
    test_randomized_scalar_equivalence();
    std::cout << "Zevryon MassiveDoc exact-byte SIMD matcher tests passed\n";
    return 0;
}
