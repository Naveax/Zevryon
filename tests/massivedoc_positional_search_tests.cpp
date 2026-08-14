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

std::filesystem::path temp_root() {
    std::mt19937_64 random(0x4d33534541524348ULL);
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("zevryon-positional-search-") +
                       std::to_string(random()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "cannot clear positional search root");
    return root;
}

void cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "positional search cleanup failed");
}

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(
        begin,
        begin + static_cast<std::ptrdiff_t>(text.size()));
}

void test_search_crosses_transfer_and_segment_boundaries() {
    const auto root = temp_root();
    zevryon::massivedoc::StoreConfig write_config;
    write_config.segment_bytes = 11U;
    write_config.records_per_search_block = 2U;
    zevryon::massivedoc::StoreWriter writer(root, write_config);
    std::string error;
    const auto first = bytes_of("alpha-record-crosses-segments-tail");
    const auto second = bytes_of("beta-unrelated-record-tail");
    const auto third = bytes_of("gamma-record-crosses-segments-again");
    require(writer.append(701U, first, &error), error);
    require(writer.append(702U, second, &error), error);
    require(writer.append(703U, third, &error), error);
    zevryon::massivedoc::StoreStats stats;
    require(writer.finalize({}, &stats, &error), error);

    zevryon::massivedoc::StoreReadConfig read_config;
    read_config.io_window_bytes = 3U;
    zevryon::massivedoc::StoreReader reader(root, read_config);
    require(reader.open(&error), error);

    const auto hits = reader.find("record-crosses-segments", 8U, &error);
    require(error.empty(), error);
    require(hits.size() == 2U, "positional search hit count mismatch");
    require(hits[0].record_index == 0U, "first positional search record index mismatch");
    require(hits[0].logical_id == 701U, "first positional search logical id mismatch");
    require(hits[0].byte_offset == 6U, "first positional search byte offset mismatch");
    require(hits[1].record_index == 2U, "second positional search record index mismatch");
    require(hits[1].logical_id == 703U, "second positional search logical id mismatch");
    require(hits[1].byte_offset == 6U, "second positional search byte offset mismatch");

    const auto limited = reader.find("record-crosses-segments", 1U, &error);
    require(error.empty(), error);
    require(limited.size() == 1U, "positional search max_hits was not enforced");
    require(limited[0].logical_id == 701U, "positional search max_hits changed ordering");

    const auto miss = reader.find("not-present-anywhere", 4U, &error);
    require(error.empty(), error);
    require(miss.empty(), "positional search false positive");
    cleanup(root);
}

} // namespace

int main() {
    test_search_crosses_transfer_and_segment_boundaries();
    std::cout << "Zevryon MassiveDoc positional search tests passed\n";
    return 0;
}
