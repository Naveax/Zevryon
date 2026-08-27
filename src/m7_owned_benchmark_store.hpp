#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace zevryon::massivedoc {

struct M7OwnedBenchmarkStoreReceipt {
    std::uint64_t record_index{0U};
    std::uint64_t payload_bytes{0U};
    std::string payload_sha256;
};

bool build_m7_owned_benchmark_store(
    const std::filesystem::path& store_root,
    std::uint64_t payload_bytes,
    M7OwnedBenchmarkStoreReceipt* receipt,
    std::string* error);

} // namespace zevryon::massivedoc
