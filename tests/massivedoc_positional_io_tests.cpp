#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "massivedoc_positional_io.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

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

std::filesystem::path temp_file(std::string_view name) {
    std::mt19937_64 random(0x4d33504f53494f55ULL);
    const auto path = std::filesystem::temp_directory_path() /
                      (std::string("zevryon-") + std::string(name) + "-" +
                       std::to_string(random()) + ".bin");
    std::error_code error;
    std::filesystem::remove(path, error);
    return path;
}

void cleanup(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    require(!error, "positional I/O fixture cleanup failed");
}

void write_sparse_marker(
    const std::filesystem::path& path,
    std::uint64_t offset,
    std::string_view marker) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0U,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    require(handle != INVALID_HANDLE_VALUE, "cannot create Windows sparse fixture");
    DWORD ignored = 0U;
    require(
        DeviceIoControl(
            handle,
            FSCTL_SET_SPARSE,
            nullptr,
            0U,
            nullptr,
            0U,
            &ignored,
            nullptr) != 0,
        "cannot mark Windows fixture sparse");
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    require(
        SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != 0,
        "cannot seek Windows sparse fixture");
    require(
        marker.size() <= static_cast<std::size_t>(std::numeric_limits<DWORD>::max()),
        "marker is too large for Windows fixture write");
    DWORD written = 0U;
    require(
        WriteFile(
            handle,
            marker.data(),
            static_cast<DWORD>(marker.size()),
            &written,
            nullptr) != 0 &&
            written == static_cast<DWORD>(marker.size()),
        "cannot write Windows sparse fixture marker");
    require(FlushFileBuffers(handle) != 0, "cannot flush Windows sparse fixture");
    require(CloseHandle(handle) != 0, "cannot close Windows sparse fixture");
#else
    static_assert(sizeof(off_t) >= sizeof(std::int64_t),
                  "positional I/O test requires 64-bit off_t");
    require(
        offset <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()),
        "sparse fixture offset exceeds off_t");
    const int descriptor = ::open(
        path.c_str(),
        O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC,
        0600);
    require(descriptor >= 0, "cannot create POSIX sparse fixture");
    std::size_t completed = 0U;
    while (completed < marker.size()) {
        const ssize_t result = ::pwrite(
            descriptor,
            marker.data() + completed,
            marker.size() - completed,
            static_cast<off_t>(offset + static_cast<std::uint64_t>(completed)));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        require(result > 0, "cannot write POSIX sparse fixture marker");
        completed += static_cast<std::size_t>(result);
    }
    require(::fsync(descriptor) == 0, "cannot flush POSIX sparse fixture");
    require(::close(descriptor) == 0, "cannot close POSIX sparse fixture");
#endif
}

void test_reads_above_four_gib_without_offset_truncation() {
    const auto path = temp_file("positional-4gib");
    constexpr std::uint64_t offset = (1ULL << 32U) + 123U;
    constexpr std::string_view marker = "M3-positional-marker";
    write_sparse_marker(path, offset, marker);

    zevryon::massivedoc::BoundedPositionalReader reader(path, 3U);
    std::string error;
    require(reader.open(&error), error);
    require(reader.file_size() == offset + marker.size(), "sparse fixture size mismatch");
    std::array<std::byte, marker.size()> output{};
    require(reader.read_exact_at(offset, output, &error), error);
    const auto* bytes = reinterpret_cast<const char*>(output.data());
    require(
        std::string_view(bytes, output.size()) == marker,
        "4 GiB positional marker mismatch");
    const auto stats = reader.stats();
    require(stats.maximum_transfer_bytes <= 3U, "positional read exceeded configured transfer window");
    require(stats.system_reads >= 2U, "bounded positional read did not split the transfer");
    require(stats.bytes_read == marker.size(), "positional byte accounting mismatch");
    cleanup(path);
}

void test_rejects_zero_window_and_out_of_range_reads() {
    const auto path = temp_file("positional-bounds");
    write_sparse_marker(path, 0U, "abcdef");

    std::string error;
    zevryon::massivedoc::BoundedPositionalReader invalid(path, 0U);
    require(!invalid.open(&error), "zero positional transfer window was accepted");

    zevryon::massivedoc::BoundedPositionalReader reader(path, 2U);
    require(reader.open(&error), error);
    std::array<std::byte, 4> output{};
    require(!reader.read_exact_at(4U, output, &error), "out-of-range positional read was accepted");
    require(
        error == "positional read exceeds file size",
        "out-of-range positional read diagnostic mismatch");
    cleanup(path);
}

void test_exact_window_accounting() {
    const auto path = temp_file("positional-window");
    constexpr std::string_view marker = "0123456789abcdef";
    write_sparse_marker(path, 0U, marker);

    zevryon::massivedoc::BoundedPositionalReader reader(path, 5U);
    std::string error;
    require(reader.open(&error), error);
    std::array<std::byte, marker.size()> output{};
    require(reader.read_exact_at(0U, output, &error), error);
    const auto stats = reader.stats();
    require(stats.system_reads == 4U, "unexpected positional system-read count");
    require(stats.maximum_transfer_bytes == 5U, "configured positional window was not enforced");
    require(stats.bytes_read == marker.size(), "positional read byte total mismatch");
    cleanup(path);
}

} // namespace

int main() {
    test_reads_above_four_gib_without_offset_truncation();
    test_rejects_zero_window_and_out_of_range_reads();
    test_exact_window_accounting();
    std::cout << "Zevryon MassiveDoc positional I/O tests passed\n";
    return 0;
}
