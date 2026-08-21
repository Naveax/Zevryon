#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

struct SharedRecordLengthAuthorityConfig {
    std::size_t max_entries{4096U};
    std::size_t max_root_key_bytes{1024U};

    bool valid() const noexcept;
};

struct SharedRecordLengthAuthorityStatus {
    std::size_t entries{0U};
    std::size_t peak_entries{0U};
    std::uint64_t cache_hits{0U};
    std::uint64_t cache_misses{0U};
    std::uint64_t insertions{0U};
    std::uint64_t replacements{0U};
    std::uint64_t evictions{0U};
    std::uint64_t resolver_failures{0U};
    std::uint64_t invalid_requests{0U};
};

using RecordLengthResolver = std::function<bool(
    const std::filesystem::path&,
    std::uint64_t,
    std::uint64_t*,
    std::string*)>;

class SharedRecordLengthAuthority final {
public:
    explicit SharedRecordLengthAuthority(
        SharedRecordLengthAuthorityConfig config = {});
    ~SharedRecordLengthAuthority();

    SharedRecordLengthAuthority(const SharedRecordLengthAuthority&) = delete;
    SharedRecordLengthAuthority& operator=(const SharedRecordLengthAuthority&) = delete;
    SharedRecordLengthAuthority(SharedRecordLengthAuthority&&) = delete;
    SharedRecordLengthAuthority& operator=(SharedRecordLengthAuthority&&) = delete;

    bool valid() const noexcept;

    bool query(
        const std::filesystem::path& store_root,
        std::uint64_t record_index,
        const RecordLengthResolver& resolver,
        std::uint64_t* record_length,
        std::string* error);

    bool try_get(
        const std::filesystem::path& store_root,
        std::uint64_t record_index,
        std::uint64_t* record_length);

    bool remember(
        const std::filesystem::path& store_root,
        std::uint64_t record_index,
        std::uint64_t record_length,
        std::string* error);

    SharedRecordLengthAuthorityStatus status() const;
    void clear() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace zevryon::massivedoc
