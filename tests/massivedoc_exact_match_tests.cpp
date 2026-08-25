#include "massivedoc_exact_match.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace zevryon::massivedoc;

[[noreturn]] void die(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        die(message);
    }
}

std::size_t scalar_reference(
    std::span<const std::byte> haystack,
    std::span<const std::byte> needle) {
    if (needle.empty()) {
        return 0U;
    }
    const auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end());
    return it == haystack.end()
        ? kExactByteMatchNotFound
        : static_cast<std::size_t>(std::distance(haystack.begin(), it));
}

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(begin, begin + text.size());
}

void test_backend_contract() {
    const ExactByteMatchBackend backend = exact_byte_match_backend();
    const std::string_view name = exact_byte_match_backend_name(backend);
    require(
        name == "scalar" || name == "sse2" || name == "neon",
        "exact matcher selected an unknown backend");
    require(
        exact_byte_match_simd_available() ==
            (backend != ExactByteMatchBackend::Scalar),
        "SIMD availability disagrees with selected backend");
}

void test_edges() {
    const std::vector<std::byte> empty;
    const std::vector<std::byte> one{std::byte{0x00}};
    require(find_exact_bytes(empty, empty) == 0U, "auto empty/empty");
    require(find_exact_bytes_scalar(empty, empty) == 0U, "scalar empty/empty");
    require(find_exact_bytes(one, empty) == 0U, "auto empty needle");
    require(find_exact_bytes_scalar(one, empty) == 0U, "scalar empty needle");
    require(
        find_exact_bytes(empty, one) == kExactByteMatchNotFound,
        "auto needle larger than haystack");
    require(
        find_exact_bytes_scalar(empty, one) == kExactByteMatchNotFound,
        "scalar needle larger than haystack");
    require(find_exact_bytes(one, one) == 0U, "auto single-byte exact");
    require(find_exact_bytes_scalar(one, one) == 0U, "scalar single-byte exact");

    const std::vector<std::byte> repeated{
        std::byte{0x61}, std::byte{0x61}, std::byte{0x61}, std::byte{0x62}};
    const std::vector<std::byte> needle{
        std::byte{0x61}, std::byte{0x61}, std::byte{0x62}};
    require(find_exact_bytes(repeated, needle) == 1U, "auto first-match overlap semantics");
    require(
        find_exact_bytes_scalar(repeated, needle) == 1U,
        "scalar first-match overlap semantics");
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
        ExactByteMatchStats auto_stats;
        ExactByteMatchStats scalar_stats;
        require(
            find_exact_bytes(haystack, needle, &auto_stats) == location,
            "auto binary boundary match");
        require(
            find_exact_bytes_scalar(haystack, needle, &scalar_stats) == location,
            "scalar binary boundary match");
        require(
            scalar_stats.simd_batches == 0U,
            "authoritative scalar matcher executed a SIMD batch");
        if (exact_byte_match_simd_available() && location >= 16U) {
            require(
                auto_stats.simd_batches != 0U,
                "SIMD backend did not execute a SIMD batch");
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
                if (!needle.empty() && needle.size() <= haystack.size() &&
                    (random() & 1U) != 0U) {
                    const std::size_t position = static_cast<std::size_t>(
                        random() % static_cast<std::uint64_t>(
                            haystack.size() - needle.size() + 1U));
                    std::copy(
                        needle.begin(), needle.end(),
                        haystack.begin() + static_cast<std::ptrdiff_t>(position));
                }

                ExactByteMatchStats auto_stats;
                ExactByteMatchStats scalar_stats;
                const std::size_t actual =
                    find_exact_bytes(haystack, needle, &auto_stats);
                const std::size_t scalar =
                    find_exact_bytes_scalar(haystack, needle, &scalar_stats);
                const std::size_t reference = scalar_reference(haystack, needle);
                if (actual != scalar || scalar != reference) {
                    std::cerr << "equivalence mismatch haystack=" << haystack_size
                              << " needle=" << needle_size
                              << " trial=" << trial
                              << " auto=" << actual
                              << " scalar=" << scalar
                              << " reference=" << reference << '\n';
                    std::exit(1);
                }
                require(
                    scalar_stats.simd_batches == 0U,
                    "scalar equivalence path executed SIMD");
            }
        }
    }
}

void test_store_reader_exact_verifier_uses_matcher() {
    const auto root = std::filesystem::temp_directory_path() /
        "zevryon-m4-exact-match-store-reader";
    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    require(!fs_error, "cannot clean integration fixture");

    StoreConfig config;
    config.segment_bytes = 256U;
    config.records_per_search_block = 1U;
    StoreWriter writer(root, config);
    const std::string record =
        std::string(20U, 'x') + "SIMD_NEEDLE" + std::string(80U, 'y');
    const auto record_bytes = bytes_of(record);
    std::string error;
    require(writer.append(77U, record_bytes, &error), "cannot append integration record");
    StoreStats store_stats;
    require(writer.finalize({}, &store_stats, &error), "cannot finalize integration store");

    StoreReadConfig read_config;
    read_config.io_window_bytes = 64U;
    StoreReader reader(root, read_config);
    require(reader.open(&error), "cannot open integration store");

    SearchExecutionStats stats;
    const auto hits = reader.find_bounded(
        "SIMD_NEEDLE",
        1U,
        {},
        &stats,
        &error);
    require(error.empty(), "StoreReader exact verifier returned an error");
    require(hits.size() == 1U, "StoreReader exact verifier hit count");
    require(hits[0].record_index == 0U, "StoreReader exact verifier record index");
    require(hits[0].logical_id == 77U, "StoreReader exact verifier logical id");
    require(hits[0].byte_offset == 20U, "StoreReader exact verifier byte offset");
    require(stats.exact_records_scanned != 0U, "StoreReader did not scan an exact record");
    require(stats.exact_compares != 0U, "StoreReader did not perform exact matcher compares");
    if (exact_byte_match_simd_available()) {
        require(
            stats.exact_simd_batches != 0U,
            "StoreReader did not execute SIMD exact matcher batches");
    }

    std::filesystem::remove_all(root, fs_error);
    require(!fs_error, "cannot remove integration fixture");
}

} // namespace

int main() {
    test_backend_contract();
    test_edges();
    test_binary_and_batch_boundaries();
    test_randomized_scalar_equivalence();
    test_store_reader_exact_verifier_uses_matcher();
    std::cout << "Zevryon MassiveDoc exact-byte scalar/SIMD matcher tests passed\n";
    return 0;
}
