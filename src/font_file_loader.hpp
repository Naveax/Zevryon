#pragma once

#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "verified_font_resource_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace zevryon::text {

enum class FontFileLoadErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    MetadataFailed,
    NotRegularFile,
    EmptyFile,
    FileTooLarge,
    StreamSizeOverflow,
    StagingBudgetExceeded,
    AllocationFailed,
    OpenFailed,
    ReadFailed,
    FileChanged,
    CacheFailed
};

struct FontFileLoadError {
    FontFileLoadErrorKind kind{FontFileLoadErrorKind::None};
    int system_error_value{0};
    VerifiedFontResourceCacheError cache_error;
    std::string message;
};

struct FontFileLoadStats {
    std::uint64_t file_bytes_before{0};
    std::uint64_t file_bytes_after{0};
    std::uint64_t exact_read_bytes{0};
    bool metadata_stable{false};
    FontContentIdentity identity;
    core::ResourceSnapshot staging;
    VerifiedFontResourceCacheStats cache;
};

const char* font_file_load_error_kind_name(
    FontFileLoadErrorKind kind) noexcept;

// Reads one regular file under an exact staging hard limit, rejects detectable
// size/write-time changes during the read, and publishes the resulting verified
// content-addressed face through the bounded cache. All outputs are atomic.
bool load_verified_font_file(
    const std::filesystem::path& path,
    std::uint32_t face_index,
    std::size_t staging_hard_limit,
    VerifiedFontResourceCache* cache,
    std::shared_ptr<const VerifiedFontResource>* output,
    FontFileLoadStats* stats,
    FontFileLoadError* error) noexcept;

} // namespace zevryon::text
