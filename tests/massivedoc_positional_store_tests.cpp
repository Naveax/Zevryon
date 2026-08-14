#include "massivedoc_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

std::filesystem::path temp_root(std::string_view name) {
    std::mt19937_64 random(0x4d3353544f524549ULL);
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("zevryon-") + std::string(name) + "-" +
                       std::to_string(random()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "cannot clear positional store test root");
    return root;
}

void cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "positional store test cleanup failed");
}

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(
        begin,
        begin + static_cast<std::ptrdiff_t>(text.size()));
}

void create_store(const std::filesystem::path& root) {
    zevryon::massivedoc::StoreConfig config;
    config.segment_bytes = 11U;
    config.records_per_search_block = 2U;
    zevryon::massivedoc::StoreWriter writer(root, config);
    std::string error;
    const auto first = bytes_of("alpha-record-crosses-segments");
    const auto second = bytes_of("beta-record-also-crosses-segments");
    require(writer.append(1001U, first, &error), error);
    require(writer.append(1002U, second, &error), error);
    zevryon::massivedoc::StoreStats stats;
    require(writer.finalize({}, &stats, &error), error);
    require(stats.corpus.logical_records == 2U, "fixture record count mismatch");
}

void test_store_reader_uses_tiny_bounded_positional_window() {
    const auto root = temp_root("positional-store-window");
    create_store(root);

    zevryon::massivedoc::StoreReadConfig read_config;
    read_config.io_window_bytes = 3U;
    zevryon::massivedoc::StoreReader reader(root, read_config);
    std::string error;
    require(reader.open(&error), error);
    require(reader.verify(&error), error);

    std::vector<std::byte> slice;
    require(reader.read_record_slice(0U, 6U, 12U, &slice, &error), error);
    const auto* slice_data = reinterpret_cast<const char*>(slice.data());
    require(
        std::string_view(slice_data, slice.size()) == "record-cross",
        "tiny-window record slice mismatch");

    std::string collected;
    require(
        reader.read_record(
            1U,
            [&collected](std::span<const std::byte> bytes) {
                collected.append(
                    reinterpret_cast<const char*>(bytes.data()),
                    bytes.size());
                return true;
            },
            &error),
        error);
    require(
        collected == "beta-record-also-crosses-segments",
        "tiny-window full record mismatch");
    cleanup(root);
}

void test_store_reader_rejects_unbounded_window_configs() {
    const auto root = temp_root("positional-store-bounds");
    create_store(root);

    std::string error;
    zevryon::massivedoc::StoreReader zero_reader(
        root,
        zevryon::massivedoc::StoreReadConfig{0U});
    require(!zero_reader.open(&error), "zero store read window was accepted");
    require(
        error == "store reader I/O window is outside the supported bounded range",
        "zero store read window diagnostic mismatch");

    zevryon::massivedoc::StoreReader oversized_reader(
        root,
        zevryon::massivedoc::StoreReadConfig{
            zevryon::massivedoc::kMaximumIoWindowBytes + 1U});
    require(!oversized_reader.open(&error), "oversized store read window was accepted");
    require(
        error == "store reader I/O window is outside the supported bounded range",
        "oversized store read window diagnostic mismatch");
    cleanup(root);
}

} // namespace

int main() {
    test_store_reader_uses_tiny_bounded_positional_window();
    test_store_reader_rejects_unbounded_window_configs();
    std::cout << "Zevryon MassiveDoc positional store tests passed\n";
    return 0;
}
