#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace zevryon::massivedoc {

struct PositionalIoStats {
    std::uint64_t system_reads{0U};
    std::uint64_t bytes_read{0U};
    std::size_t maximum_transfer_bytes{0U};
};

class BoundedPositionalReader {
public:
    BoundedPositionalReader(
        std::filesystem::path path,
        std::size_t maximum_transfer_bytes);
    ~BoundedPositionalReader();

    BoundedPositionalReader(const BoundedPositionalReader&) = delete;
    BoundedPositionalReader& operator=(const BoundedPositionalReader&) = delete;

    bool open(std::string* error);
    bool read_exact_at(
        std::uint64_t offset,
        std::span<std::byte> output,
        std::string* error) const;

    bool is_open() const noexcept;
    std::uint64_t file_size() const noexcept;
    std::size_t maximum_transfer_bytes() const noexcept;
    PositionalIoStats stats() const noexcept;

private:
    struct Impl;
    Impl* impl_{nullptr};
};

} // namespace zevryon::massivedoc
