#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace zevryon::massivedoc {

bool probe_store_record_length(
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    std::uint64_t* record_length,
    std::string* error);

} // namespace zevryon::massivedoc
