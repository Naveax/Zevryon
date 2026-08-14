#pragma once

#include "massivedoc_trigram_index.hpp"

#include <filesystem>
#include <string>

namespace zevryon::massivedoc {

bool build_store_trigram_index(
    const std::filesystem::path& root,
    TrigramIndexConfig config,
    const TrigramCancellationCheck& cancelled,
    TrigramIndexStats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
