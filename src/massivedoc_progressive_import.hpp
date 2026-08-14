#pragma once

#include "compact_document.hpp"
#include "massivedoc_store.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace zevryon::massivedoc {

struct ProgressiveImportConfig {
    StoreConfig store{};
    std::uint64_t preview_records{1U};
    ArenaConfig preview_arena{};
};

struct ProgressivePreviewInfo {
    std::filesystem::path root;
    StoreStats store;
    ArenaStats arena;
    std::uint64_t total_records{0U};
    std::uint64_t remaining_records{0U};
};

using ProgressivePreviewCallback =
    std::function<bool(const ProgressivePreviewInfo&, std::string*)>;

bool import_zmdoc_corpus_progressive(
    const std::filesystem::path& corpus_path,
    const std::filesystem::path& store_root,
    const std::filesystem::path& preview_root,
    ProgressiveImportConfig config,
    const ProgressivePreviewCallback& on_preview,
    StoreStats* final_stats,
    std::string* error);

} // namespace zevryon::massivedoc
