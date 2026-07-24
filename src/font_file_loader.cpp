#include "font_file_loader.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <system_error>
#include <utility>
#include <vector>

namespace zevryon::text {
namespace {

void clear_error(FontFileLoadError* error) noexcept {
    if (error != nullptr) {
        error->kind = FontFileLoadErrorKind::None;
        error->system_error_value = 0;
        error->cache_error = {};
        error->message.clear();
    }
}

bool fail(
    FontFileLoadErrorKind kind,
    const char* message,
    int system_error_value,
    FontFileLoadError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->system_error_value = system_error_value;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

int error_value(const std::error_code& error) noexcept {
    return error ? error.value() : 0;
}

} // namespace

const char* font_file_load_error_kind_name(
    FontFileLoadErrorKind kind) noexcept {
    switch (kind) {
    case FontFileLoadErrorKind::None:
        return "none";
    case FontFileLoadErrorKind::InvalidArgument:
        return "invalid_argument";
    case FontFileLoadErrorKind::MetadataFailed:
        return "metadata_failed";
    case FontFileLoadErrorKind::NotRegularFile:
        return "not_regular_file";
    case FontFileLoadErrorKind::EmptyFile:
        return "empty_file";
    case FontFileLoadErrorKind::FileTooLarge:
        return "file_too_large";
    case FontFileLoadErrorKind::StreamSizeOverflow:
        return "stream_size_overflow";
    case FontFileLoadErrorKind::StagingBudgetExceeded:
        return "staging_budget_exceeded";
    case FontFileLoadErrorKind::AllocationFailed:
        return "allocation_failed";
    case FontFileLoadErrorKind::OpenFailed:
        return "open_failed";
    case FontFileLoadErrorKind::ReadFailed:
        return "read_failed";
    case FontFileLoadErrorKind::FileChanged:
        return "file_changed";
    case FontFileLoadErrorKind::CacheFailed:
        return "cache_failed";
    }
    return "unknown";
}

bool load_verified_font_file(
    const std::filesystem::path& path,
    std::uint32_t face_index,
    std::size_t staging_hard_limit,
    VerifiedFontResourceCache* cache,
    std::shared_ptr<const VerifiedFontResource>* output,
    FontFileLoadStats* stats,
    FontFileLoadError* error) noexcept {
    if (output != nullptr) {
        output->reset();
    }
    if (stats != nullptr) {
        *stats = {};
    }
    clear_error(error);

    if (path.empty() || staging_hard_limit == 0U || cache == nullptr ||
        output == nullptr || stats == nullptr || error == nullptr) {
        return fail(
            FontFileLoadErrorKind::InvalidArgument,
            "path, staging limit, cache, output, stats, and error are required",
            0,
            error);
    }

    std::error_code filesystem_error;
    const std::filesystem::file_status initial_status =
        std::filesystem::status(path, filesystem_error);
    if (filesystem_error) {
        return fail(
            FontFileLoadErrorKind::MetadataFailed,
            "font file status could not be read",
            error_value(filesystem_error),
            error);
    }
    if (!std::filesystem::exists(initial_status)) {
        return fail(
            FontFileLoadErrorKind::MetadataFailed,
            "font file does not exist",
            0,
            error);
    }
    if (!std::filesystem::is_regular_file(initial_status)) {
        return fail(
            FontFileLoadErrorKind::NotRegularFile,
            "font path is not a regular file",
            0,
            error);
    }

    const std::uintmax_t size_before =
        std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        return fail(
            FontFileLoadErrorKind::MetadataFailed,
            "font file size could not be read",
            error_value(filesystem_error),
            error);
    }
    const std::filesystem::file_time_type write_time_before =
        std::filesystem::last_write_time(path, filesystem_error);
    if (filesystem_error) {
        return fail(
            FontFileLoadErrorKind::MetadataFailed,
            "font file write time could not be read",
            error_value(filesystem_error),
            error);
    }

    stats->file_bytes_before = size_before >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::uint64_t>::max())
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(size_before);
    if (size_before == 0U) {
        return fail(
            FontFileLoadErrorKind::EmptyFile,
            "font file is empty",
            0,
            error);
    }
    if (size_before > static_cast<std::uintmax_t>(staging_hard_limit) ||
        size_before > static_cast<std::uintmax_t>(
                          std::numeric_limits<std::size_t>::max())) {
        return fail(
            FontFileLoadErrorKind::FileTooLarge,
            "font file exceeds the staging hard limit",
            0,
            error);
    }
    if (size_before > static_cast<std::uintmax_t>(
                          std::numeric_limits<std::streamsize>::max())) {
        return fail(
            FontFileLoadErrorKind::StreamSizeOverflow,
            "font file exceeds the streamsize read contract",
            0,
            error);
    }

    core::ResourceLedger staging_ledger;
    staging_ledger.set_hard_limit(
        core::ResourceClass::FontFileReadBuffer,
        staging_hard_limit);
    core::LedgerMemoryResource staging_resource(
        staging_ledger,
        core::ResourceClass::FontFileReadBuffer);

    std::shared_ptr<const VerifiedFontResource> candidate;
    FontContentIdentity identity;
    VerifiedFontResourceCacheStats cache_stats;
    bool operation_succeeded = false;

    try {
        operation_succeeded = [&]() -> bool {
            std::pmr::vector<std::byte> bytes(&staging_resource);
            try {
                bytes.resize(static_cast<std::size_t>(size_before));
            } catch (const std::bad_alloc&) {
                const bool rejected = staging_ledger.snapshot(
                    core::ResourceClass::FontFileReadBuffer)
                                          .rejected_reservations != 0U;
                return fail(
                    rejected
                        ? FontFileLoadErrorKind::StagingBudgetExceeded
                        : FontFileLoadErrorKind::AllocationFailed,
                    rejected
                        ? "font staging buffer exceeded its hard limit"
                        : "font staging buffer allocation failed",
                    0,
                    error);
            } catch (...) {
                return fail(
                    FontFileLoadErrorKind::AllocationFailed,
                    "font staging buffer allocation failed",
                    0,
                    error);
            }

            std::ifstream stream(path, std::ios::binary);
            if (!stream.is_open()) {
                return fail(
                    FontFileLoadErrorKind::OpenFailed,
                    "font file could not be opened",
                    0,
                    error);
            }

            const std::streamsize expected =
                static_cast<std::streamsize>(bytes.size());
            stream.read(
                reinterpret_cast<char*>(bytes.data()),
                expected);
            const std::streamsize actual = stream.gcount();
            if (actual > 0) {
                staging_ledger.record_physical_read(
                    core::ResourceClass::FontFileReadBuffer,
                    static_cast<std::uint64_t>(actual));
                stats->exact_read_bytes = static_cast<std::uint64_t>(actual);
            }
            if (actual != expected) {
                return fail(
                    FontFileLoadErrorKind::ReadFailed,
                    "font file did not produce the exact preflight byte count",
                    0,
                    error);
            }

            char extra = 0;
            stream.read(&extra, 1);
            if (stream.bad()) {
                return fail(
                    FontFileLoadErrorKind::ReadFailed,
                    "font file produced an I/O error after the exact read",
                    0,
                    error);
            }
            if (stream.gcount() != 0) {
                staging_ledger.record_physical_read(
                    core::ResourceClass::FontFileReadBuffer,
                    1U);
                return fail(
                    FontFileLoadErrorKind::FileChanged,
                    "font file grew during the bounded read",
                    0,
                    error);
            }

            filesystem_error.clear();
            const std::uintmax_t size_after =
                std::filesystem::file_size(path, filesystem_error);
            if (filesystem_error) {
                return fail(
                    FontFileLoadErrorKind::MetadataFailed,
                    "font file size could not be re-read",
                    error_value(filesystem_error),
                    error);
            }
            const std::filesystem::file_time_type write_time_after =
                std::filesystem::last_write_time(path, filesystem_error);
            if (filesystem_error) {
                return fail(
                    FontFileLoadErrorKind::MetadataFailed,
                    "font file write time could not be re-read",
                    error_value(filesystem_error),
                    error);
            }
            stats->file_bytes_after = size_after >
                    static_cast<std::uintmax_t>(
                        std::numeric_limits<std::uint64_t>::max())
                ? std::numeric_limits<std::uint64_t>::max()
                : static_cast<std::uint64_t>(size_after);
            if (size_after != size_before ||
                write_time_after != write_time_before) {
                return fail(
                    FontFileLoadErrorKind::FileChanged,
                    "font file metadata changed during the read",
                    0,
                    error);
            }
            stats->metadata_stable = true;

            VerifiedFontResourceCacheError cache_error;
            if (!cache->get_or_build_content_addressed(
                    bytes,
                    face_index,
                    &candidate,
                    &identity,
                    &cache_stats,
                    &cache_error)) {
                error->kind = FontFileLoadErrorKind::CacheFailed;
                error->system_error_value = 0;
                error->cache_error = std::move(cache_error);
                try {
                    error->message = "verified font cache publication failed: ";
                    error->message +=
                        verified_font_resource_cache_error_kind_name(
                            error->cache_error.kind);
                } catch (...) {
                    error->message.clear();
                }
                return false;
            }
            return true;
        }();
    } catch (const std::bad_alloc&) {
        operation_succeeded = fail(
            FontFileLoadErrorKind::AllocationFailed,
            "font file loading allocation failed",
            0,
            error);
    } catch (...) {
        operation_succeeded = fail(
            FontFileLoadErrorKind::ReadFailed,
            "font file loading failed unexpectedly",
            0,
            error);
    }

    stats->staging = staging_ledger.snapshot(
        core::ResourceClass::FontFileReadBuffer);
    stats->cache = cache_stats;
    stats->identity = identity;
    if (!operation_succeeded) {
        candidate.reset();
        return false;
    }
    if (stats->staging.current_bytes != 0U ||
        stats->staging.accounting_errors != 0U) {
        candidate.reset();
        return fail(
            FontFileLoadErrorKind::AllocationFailed,
            "font staging buffer accounting did not return to zero",
            0,
            error);
    }

    *output = std::move(candidate);
    return true;
}

} // namespace zevryon::text
