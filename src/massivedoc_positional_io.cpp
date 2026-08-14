#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "massivedoc_positional_io.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <utility>

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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace zevryon::massivedoc {
namespace {

void record_maximum_transfer(
    std::atomic<std::size_t>* maximum,
    std::size_t amount) noexcept {
    std::size_t observed = maximum->load(std::memory_order_relaxed);
    while (observed < amount &&
           !maximum->compare_exchange_weak(
               observed,
               amount,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

} // namespace

struct BoundedPositionalReader::Impl {
    std::filesystem::path path;
    std::size_t configured_maximum_transfer_bytes{0U};
    std::size_t effective_maximum_transfer_bytes{0U};
    std::uint64_t file_bytes{0U};
    std::atomic<std::uint64_t> system_reads{0U};
    std::atomic<std::uint64_t> bytes_read{0U};
    std::atomic<std::size_t> maximum_observed_transfer_bytes{0U};
#if defined(_WIN32)
    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    int descriptor{-1};
#endif

    bool opened() const noexcept {
#if defined(_WIN32)
        return handle != INVALID_HANDLE_VALUE;
#else
        return descriptor >= 0;
#endif
    }

    void close() noexcept {
#if defined(_WIN32)
        if (handle != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor >= 0) {
            (void)::close(descriptor);
            descriptor = -1;
        }
#endif
        file_bytes = 0U;
    }

    bool read_once(
        std::uint64_t offset,
        std::span<std::byte> output,
        std::size_t* received,
        std::string* error) const {
        if (received == nullptr || error == nullptr) {
            return false;
        }
        *received = 0U;
        if (output.empty()) {
            return true;
        }
#if defined(_WIN32)
        if (output.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
            *error = "positional read transfer exceeds Windows DWORD range";
            return false;
        }
        const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) {
            *error = "cannot create positional read completion event: " +
                     std::to_string(static_cast<unsigned long>(GetLastError()));
            return false;
        }
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffULL);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
        overlapped.hEvent = event;
        const DWORD request = static_cast<DWORD>(output.size());
        BOOL started = ReadFile(
            handle,
            output.data(),
            request,
            nullptr,
            &overlapped);
        DWORD transferred = 0U;
        if (started == 0) {
            const DWORD start_error = GetLastError();
            if (start_error != ERROR_IO_PENDING) {
                (void)CloseHandle(event);
                if (start_error == ERROR_HANDLE_EOF) {
                    return true;
                }
                *error = "positional ReadFile failed: " +
                         std::to_string(static_cast<unsigned long>(start_error));
                return false;
            }
            started = GetOverlappedResult(handle, &overlapped, &transferred, TRUE);
        } else {
            started = GetOverlappedResult(handle, &overlapped, &transferred, FALSE);
        }
        const DWORD completion_error = started == 0 ? GetLastError() : ERROR_SUCCESS;
        (void)CloseHandle(event);
        if (started == 0) {
            if (completion_error == ERROR_HANDLE_EOF) {
                return true;
            }
            *error = "positional ReadFile completion failed: " +
                     std::to_string(static_cast<unsigned long>(completion_error));
            return false;
        }
        *received = static_cast<std::size_t>(transferred);
        return true;
#else
        static_assert(sizeof(off_t) >= sizeof(std::int64_t),
                      "MassiveDoc positional I/O requires 64-bit off_t");
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            *error = "positional read offset exceeds off_t range";
            return false;
        }
        const std::size_t platform_maximum =
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
        if (output.size() > platform_maximum) {
            *error = "positional read transfer exceeds ssize_t range";
            return false;
        }
        ssize_t result = -1;
        do {
            result = ::pread(
                descriptor,
                output.data(),
                output.size(),
                static_cast<off_t>(offset));
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            *error = "positional pread failed with errno " + std::to_string(errno);
            return false;
        }
        *received = static_cast<std::size_t>(result);
        return true;
#endif
    }
};

BoundedPositionalReader::BoundedPositionalReader(
    std::filesystem::path path,
    std::size_t maximum_transfer_bytes)
    : impl_(new Impl{}) {
    impl_->path = std::move(path);
    impl_->configured_maximum_transfer_bytes = maximum_transfer_bytes;
}

BoundedPositionalReader::~BoundedPositionalReader() {
    if (impl_ != nullptr) {
        impl_->close();
    }
    delete impl_;
}

bool BoundedPositionalReader::open(std::string* error) {
    if (impl_ == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->configured_maximum_transfer_bytes == 0U) {
        *error = "positional read maximum transfer must be non-zero";
        return false;
    }
    impl_->close();
#if defined(_WIN32)
    const std::size_t platform_maximum =
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max());
    impl_->effective_maximum_transfer_bytes =
        std::min(impl_->configured_maximum_transfer_bytes, platform_maximum);
    impl_->handle = CreateFileW(
        impl_->path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        *error = "cannot open positional read file: " +
                 std::to_string(static_cast<unsigned long>(GetLastError()));
        return false;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(impl_->handle, &size) == 0 || size.QuadPart < 0) {
        const DWORD size_error = GetLastError();
        impl_->close();
        *error = "cannot size positional read file: " +
                 std::to_string(static_cast<unsigned long>(size_error));
        return false;
    }
    impl_->file_bytes = static_cast<std::uint64_t>(size.QuadPart);
#else
    const std::size_t platform_maximum =
        static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
    impl_->effective_maximum_transfer_bytes =
        std::min(impl_->configured_maximum_transfer_bytes, platform_maximum);
    impl_->descriptor = ::open(impl_->path.c_str(), O_RDONLY | O_CLOEXEC);
    if (impl_->descriptor < 0) {
        *error = "cannot open positional read file with errno " + std::to_string(errno);
        return false;
    }
    struct stat status {};
    if (::fstat(impl_->descriptor, &status) != 0 || status.st_size < 0) {
        const int status_error = errno;
        impl_->close();
        *error = "cannot size positional read file with errno " +
                 std::to_string(status_error);
        return false;
    }
    impl_->file_bytes = static_cast<std::uint64_t>(status.st_size);
#endif
    impl_->system_reads.store(0U, std::memory_order_relaxed);
    impl_->bytes_read.store(0U, std::memory_order_relaxed);
    impl_->maximum_observed_transfer_bytes.store(0U, std::memory_order_relaxed);
    return true;
}

bool BoundedPositionalReader::read_exact_at(
    std::uint64_t offset,
    std::span<std::byte> output,
    std::string* error) const {
    if (impl_ == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    if (!impl_->opened()) {
        *error = "positional reader is not open";
        return false;
    }
    if (output.size() > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
        *error = "positional read size exceeds 64-bit range";
        return false;
    }
    const std::uint64_t requested = static_cast<std::uint64_t>(output.size());
    if (offset > impl_->file_bytes || requested > impl_->file_bytes - offset) {
        *error = "positional read exceeds file size";
        return false;
    }
    std::size_t completed = 0U;
    while (completed < output.size()) {
        const std::size_t amount = std::min(
            impl_->effective_maximum_transfer_bytes,
            output.size() - completed);
        std::size_t received = 0U;
        if (!impl_->read_once(
                offset + static_cast<std::uint64_t>(completed),
                output.subspan(completed, amount),
                &received,
                error)) {
            return false;
        }
        if (received != amount) {
            *error = "positional read returned a truncated transfer";
            return false;
        }
        impl_->system_reads.fetch_add(1U, std::memory_order_relaxed);
        impl_->bytes_read.fetch_add(
            static_cast<std::uint64_t>(received),
            std::memory_order_relaxed);
        record_maximum_transfer(
            &impl_->maximum_observed_transfer_bytes,
            received);
        completed += received;
    }
    return true;
}

bool BoundedPositionalReader::is_open() const noexcept {
    return impl_ != nullptr && impl_->opened();
}

std::uint64_t BoundedPositionalReader::file_size() const noexcept {
    return impl_ != nullptr ? impl_->file_bytes : 0U;
}

std::size_t BoundedPositionalReader::maximum_transfer_bytes() const noexcept {
    return impl_ != nullptr ? impl_->effective_maximum_transfer_bytes : 0U;
}

PositionalIoStats BoundedPositionalReader::stats() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    return PositionalIoStats{
        impl_->system_reads.load(std::memory_order_relaxed),
        impl_->bytes_read.load(std::memory_order_relaxed),
        impl_->maximum_observed_transfer_bytes.load(std::memory_order_relaxed)};
}

} // namespace zevryon::massivedoc
