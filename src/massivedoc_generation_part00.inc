#include "massivedoc_generation.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace zevryon::massivedoc {
namespace {

constexpr std::array<char, 8> kGenerationMagic{'Z', 'M', 'D', 'G', 'E', 'N', '0', '1'};
constexpr std::array<char, 8> kJournalMagic{'Z', 'M', 'D', 'J', 'N', 'L', '0', '1'};
constexpr std::uint32_t kGenerationVersion = 1U;
constexpr std::uint32_t kJournalVersion = 1U;
constexpr std::size_t kGenerationHeaderBytes = 80U;
constexpr std::size_t kGenerationInventoryBytes = 16U;
constexpr std::size_t kGenerationTrailerBytes = sizeof(std::uint32_t);
constexpr std::size_t kJournalRecordBytes = 52U;
constexpr std::size_t kJournalPayloadBytes = kJournalRecordBytes - sizeof(std::uint32_t);
constexpr std::size_t kGenerationDigits = 16U;
constexpr std::uint64_t kMaxAuthorityManifestBytes = 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxGenerationSegments = 1024ULL * 1024ULL;
constexpr std::size_t kCopyWindowBytes = 64U * 1024U;

enum class JournalState : std::uint32_t {
    prepare = 1U,
    commit = 2U,
};

struct JournalFingerprint {
    std::uint32_t manifest_crc{0U};
    std::uint64_t manifest_bytes{0U};

    friend bool operator==(const JournalFingerprint&, const JournalFingerprint&) = default;
};

struct JournalGeneration {
    std::optional<JournalFingerprint> prepare;
    std::optional<JournalFingerprint> commit;
};

struct ParsedGeneration {
    std::uint64_t generation{0U};
    std::array<std::uint8_t, 32> source_identity{};
    std::vector<std::byte> authority_manifest;
    std::vector<GenerationSegmentInventory> segments;
};

std::uint32_t generation_crc32_update(
    std::uint32_t crc,
    std::span<const std::byte> bytes) noexcept {
    crc = ~crc;
    for (const std::byte raw : bytes) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(raw));
        for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

template <typename T>
void generation_append_le(std::vector<std::byte>* output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        output->push_back(
            static_cast<std::byte>((value >> (index * 8U)) & static_cast<T>(0xffU)));
    }
}

template <typename T>
std::optional<T> generation_read_le(std::span<const std::byte> bytes, std::size_t offset) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
        return std::nullopt;
    }
    T value = 0U;
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

bool generation_write_all(std::ostream& stream, std::span<const std::byte> bytes) {
    if (bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return false;
    }
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

bool generation_read_exact(std::istream& stream, std::span<std::byte> bytes) {
    if (bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return false;
    }
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return stream.gcount() == static_cast<std::streamsize>(bytes.size());
}

std::filesystem::path generation_root(const std::filesystem::path& store_root) {
    return store_root / "generations";
}

std::filesystem::path generation_quarantine_root(const std::filesystem::path& store_root) {
    return store_root / "quarantine";
}

std::string generation_file_name(std::uint64_t generation, bool temporary) {
    std::ostringstream output;
    output << "generation-" << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(static_cast<int>(kGenerationDigits)) << generation << ".zmg";
    if (temporary) {
        output << ".tmp";
    }
    return output.str();
}

std::filesystem::path generation_temporary_path(
    const std::filesystem::path& store_root,
    std::uint64_t generation) {
    return generation_root(store_root) / generation_file_name(generation, true);
}

bool generation_ensure_directory(
    const std::filesystem::path& path,
    std::string_view diagnostic,
    std::string* error) {
    std::error_code create_error;
    std::filesystem::create_directories(path, create_error);
    if (create_error) {
        *error = std::string(diagnostic) + ": " + create_error.message();
        return false;
    }
    return true;
}

bool generation_durable_flush_file(const std::filesystem::path& path, std::string* error) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = "cannot open generation file for durable flush";
        return false;
    }
    const BOOL flushed = FlushFileBuffers(handle);
    const BOOL closed = CloseHandle(handle);
    if (flushed == 0 || closed == 0) {
        *error = "cannot durably flush generation file";
        return false;
    }
    return true;
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        *error = "cannot open generation file for durable flush";
        return false;
    }
    const int flush_result = ::fsync(descriptor);
    const int close_result = ::close(descriptor);
    if (flush_result != 0 || close_result != 0) {
        *error = "cannot durably flush generation file";
        return false;
    }
    return true;
#endif
}

bool generation_durable_sync_directory(
    const std::filesystem::path& path,
    std::string* error) {
#if defined(_WIN32)
    static_cast<void>(path);
    static_cast<void>(error);
    return true;
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        *error = "cannot open generation directory for durable sync";
        return false;
    }
    const int sync_result = ::fsync(descriptor);
    const int close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        *error = "cannot durably sync generation directory";
        return false;
    }
    return true;
#endif
}

bool generation_publish_no_replace(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& final_path,
    std::string* error) {
#if defined(_WIN32)
    if (MoveFileExW(
            temporary_path.c_str(),
            final_path.c_str(),
            MOVEFILE_WRITE_THROUGH) == 0) {
        *error = "cannot commit generation file without replacement";
        return false;
    }
    return true;
#else
    if (::link(temporary_path.c_str(), final_path.c_str()) != 0) {
        *error = "cannot commit generation file without replacement";
        return false;
    }
    if (!generation_durable_sync_directory(final_path.parent_path(), error)) {
        return false;
    }
    std::error_code cleanup_error;
    static_cast<void>(std::filesystem::remove(temporary_path, cleanup_error));
    return true;
#endif
}

bool generation_replace_file(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& final_path,
    std::string* error) {
#if defined(_WIN32)
    if (MoveFileExW(
            temporary_path.c_str(),
            final_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        *error = "cannot replace generation journal";
        return false;
    }
    return true;
#else
    if (::rename(temporary_path.c_str(), final_path.c_str()) != 0) {
        *error = "cannot replace generation journal";
        return false;
    }
    return generation_durable_sync_directory(final_path.parent_path(), error);
#endif
}

bool generation_write_durable_temp(
    const std::filesystem::path& path,
    std::span<const std::byte> bytes,
    std::string* error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || !generation_write_all(stream, bytes)) {
        *error = "cannot write generation temporary file";
        return false;
    }
    stream.flush();
    if (!stream) {
        *error = "cannot flush generation temporary file";
        return false;
    }
    stream.close();
    if (stream.fail()) {
        *error = "cannot close generation temporary file";
        return false;
    }
    return generation_durable_flush_file(path, error);
}

bool generation_remove_stale_temp(
    const std::filesystem::path& path,
    std::string* error) {
    std::error_code status_error;
    const bool exists = std::filesystem::exists(path, status_error);
    if (status_error) {
        *error = "cannot inspect generation temporary file";
        return false;
    }
    if (!exists) {
        return true;
    }
    std::error_code remove_error;
    if (!std::filesystem::remove(path, remove_error) || remove_error) {
        *error = "cannot clear stale generation temporary file";
        return false;
    }
    return true;
}

#include "massivedoc_generation_part01.inc"
