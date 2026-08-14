#include "massivedoc_cold_window.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace zevryon::massivedoc {
namespace {

constexpr auto kSourceWindow = zevryon::core::ResourceClass::SourceWindow;

bool add_fits_u64(
    std::uint64_t offset,
    std::size_t bytes,
    std::uint64_t* end) noexcept {
    if (bytes > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
        return false;
    }
    const std::uint64_t amount = static_cast<std::uint64_t>(bytes);
    if (offset > std::numeric_limits<std::uint64_t>::max() - amount) {
        return false;
    }
    *end = offset + amount;
    return true;
}

} // namespace

struct ColdMappedWindow::Impl {
    explicit Impl(std::size_t bytes) : configured_bytes(bytes) {
        ledger.set_hard_limit(kSourceWindow, configured_bytes);
    }

    std::size_t configured_bytes{0U};
    mutable std::mutex mutex;
    zevryon::core::ResourceLedger ledger;
    std::filesystem::path mapped_path;
    const std::byte* view{nullptr};
    std::uint64_t mapped_offset{0U};
    std::size_t mapped_bytes{0U};
    std::uint64_t mapping_hits{0U};
    std::uint64_t mapping_misses{0U};
    std::uint64_t remaps{0U};
    std::uint64_t touches{0U};
    std::uint64_t touched_bytes{0U};
    volatile std::uint8_t touch_sink{0U};

    bool contains(
        const std::filesystem::path& path,
        std::uint64_t offset,
        std::size_t bytes) const noexcept {
        if (view == nullptr || mapped_path != path || offset < mapped_offset) {
            return false;
        }
        std::uint64_t request_end = 0U;
        if (!add_fits_u64(offset, bytes, &request_end)) {
            return false;
        }
        if (mapped_bytes > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
            return false;
        }
        const std::uint64_t mapped_end =
            mapped_offset + static_cast<std::uint64_t>(mapped_bytes);
        return request_end <= mapped_end;
    }

    void release_locked(bool count_eviction) noexcept {
        if (view == nullptr) {
            return;
        }
#if defined(_WIN32)
        (void)UnmapViewOfFile(view);
#else
        (void)::munmap(
            const_cast<std::byte*>(view),
            mapped_bytes);
#endif
        ledger.release(kSourceWindow, mapped_bytes);
        if (count_eviction) {
            ledger.record_eviction(kSourceWindow);
        }
        view = nullptr;
        mapped_path.clear();
        mapped_offset = 0U;
        mapped_bytes = 0U;
    }

    bool map_locked(
        const std::filesystem::path& path,
        std::uint64_t offset,
        std::size_t bytes,
        std::string* error) {
        std::uint64_t request_end = 0U;
        if (!add_fits_u64(offset, bytes, &request_end)) {
            *error = "cold mapped window request range overflow";
            return false;
        }

        // A single cold window is authoritative. Release the old mapping before
        // reserving the next one so the hard limit is never transiently doubled.
        release_locked(true);

#if defined(_WIN32)
        HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            *error = "cannot open cold mapped segment: " +
                     std::to_string(static_cast<unsigned long>(GetLastError()));
            return false;
        }
        LARGE_INTEGER file_size_value{};
        if (GetFileSizeEx(file, &file_size_value) == 0 || file_size_value.QuadPart < 0) {
            const DWORD size_error = GetLastError();
            (void)CloseHandle(file);
            *error = "cannot size cold mapped segment: " +
                     std::to_string(static_cast<unsigned long>(size_error));
            return false;
        }
        const std::uint64_t file_size =
            static_cast<std::uint64_t>(file_size_value.QuadPart);
        SYSTEM_INFO system_info{};
        GetSystemInfo(&system_info);
        const std::uint64_t granularity =
            static_cast<std::uint64_t>(system_info.dwAllocationGranularity);
        if (granularity == 0U || request_end > file_size) {
            (void)CloseHandle(file);
            *error = request_end > file_size
                ? "cold mapped window request exceeds segment size"
                : "Windows allocation granularity is unavailable";
            return false;
        }
        const std::uint64_t map_offset = offset - (offset % granularity);
        const std::uint64_t prefix = offset - map_offset;
        if (prefix > std::numeric_limits<std::uint64_t>::max() -
                         static_cast<std::uint64_t>(bytes)) {
            (void)CloseHandle(file);
            *error = "cold mapped window aligned size overflow";
            return false;
        }
        const std::uint64_t required = prefix + static_cast<std::uint64_t>(bytes);
        if (required > configured_bytes) {
            (void)CloseHandle(file);
            *error = "cold mapped window request plus alignment exceeds configured bound";
            return false;
        }
        const std::uint64_t available = file_size - map_offset;
        const std::uint64_t desired_u64 = std::min<std::uint64_t>(
            available,
            static_cast<std::uint64_t>(configured_bytes));
        const std::size_t desired = static_cast<std::size_t>(desired_u64);
        if (!ledger.try_reserve(kSourceWindow, desired)) {
            (void)CloseHandle(file);
            *error = "cold mapped window ledger rejected mapping";
            return false;
        }
        HANDLE mapping = CreateFileMappingW(
            file,
            nullptr,
            PAGE_READONLY,
            0U,
            0U,
            nullptr);
        if (mapping == nullptr) {
            const DWORD mapping_error = GetLastError();
            ledger.release(kSourceWindow, desired);
            (void)CloseHandle(file);
            *error = "cannot create cold mapped file mapping: " +
                     std::to_string(static_cast<unsigned long>(mapping_error));
            return false;
        }
        const DWORD offset_low = static_cast<DWORD>(map_offset & 0xffffffffULL);
        const DWORD offset_high = static_cast<DWORD>(map_offset >> 32U);
        const void* mapped = MapViewOfFile(
            mapping,
            FILE_MAP_READ,
            offset_high,
            offset_low,
            desired);
        const DWORD view_error = mapped == nullptr ? GetLastError() : ERROR_SUCCESS;
        (void)CloseHandle(mapping);
        (void)CloseHandle(file);
        if (mapped == nullptr) {
            ledger.release(kSourceWindow, desired);
            *error = "cannot map cold segment view: " +
                     std::to_string(static_cast<unsigned long>(view_error));
            return false;
        }
#else
        const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) {
            *error = "cannot open cold mapped segment with errno " +
                     std::to_string(errno);
            return false;
        }
        struct stat status {};
        if (::fstat(descriptor, &status) != 0 || status.st_size < 0) {
            const int status_error = errno;
            (void)::close(descriptor);
            *error = "cannot size cold mapped segment with errno " +
                     std::to_string(status_error);
            return false;
        }
        const std::uint64_t file_size = static_cast<std::uint64_t>(status.st_size);
        const long page_size_raw = ::sysconf(_SC_PAGE_SIZE);
        if (page_size_raw <= 0 || request_end > file_size) {
            (void)::close(descriptor);
            *error = request_end > file_size
                ? "cold mapped window request exceeds segment size"
                : "POSIX page size is unavailable";
            return false;
        }
        const std::uint64_t granularity = static_cast<std::uint64_t>(page_size_raw);
        const std::uint64_t map_offset = offset - (offset % granularity);
        const std::uint64_t prefix = offset - map_offset;
        if (prefix > std::numeric_limits<std::uint64_t>::max() -
                         static_cast<std::uint64_t>(bytes)) {
            (void)::close(descriptor);
            *error = "cold mapped window aligned size overflow";
            return false;
        }
        const std::uint64_t required = prefix + static_cast<std::uint64_t>(bytes);
        if (required > configured_bytes ||
            map_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            (void)::close(descriptor);
            *error = required > configured_bytes
                ? "cold mapped window request plus alignment exceeds configured bound"
                : "cold mapped window offset exceeds off_t range";
            return false;
        }
        const std::uint64_t available = file_size - map_offset;
        const std::uint64_t desired_u64 = std::min<std::uint64_t>(
            available,
            static_cast<std::uint64_t>(configured_bytes));
        const std::size_t desired = static_cast<std::size_t>(desired_u64);
        if (!ledger.try_reserve(kSourceWindow, desired)) {
            (void)::close(descriptor);
            *error = "cold mapped window ledger rejected mapping";
            return false;
        }
        void* mapped = ::mmap(
            nullptr,
            desired,
            PROT_READ,
            MAP_SHARED,
            descriptor,
            static_cast<off_t>(map_offset));
        const int mapping_error = errno;
        (void)::close(descriptor);
        if (mapped == MAP_FAILED) {
            ledger.release(kSourceWindow, desired);
            *error = "cannot map cold segment view with errno " +
                     std::to_string(mapping_error);
            return false;
        }
#endif

        mapped_path = path;
        view = static_cast<const std::byte*>(mapped);
        mapped_offset = map_offset;
        mapped_bytes = desired;
        ++remaps;
        return true;
    }
};

bool validate_cold_mapped_window_bytes(
    std::size_t window_bytes,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (window_bytes > kMaximumColdMappedWindowBytes) {
        *error = "cold mapped window exceeds the supported hard cap";
        return false;
    }
    return true;
}

ColdMappedWindow::ColdMappedWindow(std::size_t window_bytes)
    : impl_(std::make_unique<Impl>(window_bytes)) {}

ColdMappedWindow::~ColdMappedWindow() {
    release();
}

bool ColdMappedWindow::touch(
    const std::filesystem::path& path,
    std::uint64_t offset,
    std::size_t bytes,
    std::string* error) {
    if (impl_ == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    if (bytes == 0U) {
        return true;
    }
    if (impl_->configured_bytes == 0U) {
        *error = "cold mapped window is disabled";
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->contains(path, offset, bytes)) {
        ++impl_->mapping_hits;
        impl_->ledger.record_cache_hit(kSourceWindow);
    } else {
        ++impl_->mapping_misses;
        impl_->ledger.record_cache_miss(kSourceWindow);
        if (!impl_->map_locked(path, offset, bytes, error)) {
            return false;
        }
    }

    const std::uint64_t relative_u64 = offset - impl_->mapped_offset;
    if (relative_u64 > std::numeric_limits<std::size_t>::max()) {
        *error = "cold mapped window relative offset exceeds size_t";
        return false;
    }
    const std::size_t relative = static_cast<std::size_t>(relative_u64);
#if defined(_WIN32)
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const std::size_t page_stride = std::max<std::size_t>(
        static_cast<std::size_t>(system_info.dwPageSize),
        1U);
#else
    const long page_size_raw = ::sysconf(_SC_PAGE_SIZE);
    const std::size_t page_stride = page_size_raw > 0
        ? static_cast<std::size_t>(page_size_raw)
        : 4096U;
#endif
    std::uint8_t sink = 0U;
    for (std::size_t index = 0U; index < bytes; index += page_stride) {
        sink ^= std::to_integer<std::uint8_t>(impl_->view[relative + index]);
    }
    sink ^= std::to_integer<std::uint8_t>(impl_->view[relative + bytes - 1U]);
    impl_->touch_sink = sink;
    ++impl_->touches;
    if (impl_->touched_bytes >
        std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(bytes)) {
        impl_->touched_bytes = std::numeric_limits<std::uint64_t>::max();
    } else {
        impl_->touched_bytes += static_cast<std::uint64_t>(bytes);
    }
    impl_->ledger.record_physical_read(
        kSourceWindow,
        static_cast<std::uint64_t>(bytes));
    return true;
}

void ColdMappedWindow::release() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->release_locked(true);
}

ColdMappedWindowStats ColdMappedWindow::stats() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto source = impl_->ledger.snapshot(kSourceWindow);
    return ColdMappedWindowStats{
        source.current_bytes,
        source.peak_bytes,
        impl_->mapping_hits,
        impl_->mapping_misses,
        impl_->remaps,
        impl_->touches,
        impl_->touched_bytes,
        impl_->ledger.within_hard_limits(),
        impl_->ledger.accounting_clean()};
}

std::size_t ColdMappedWindow::window_bytes() const noexcept {
    return impl_ != nullptr ? impl_->configured_bytes : 0U;
}

} // namespace zevryon::massivedoc
