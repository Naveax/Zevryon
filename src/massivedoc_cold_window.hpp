#pragma once

#include "massivedoc_address_space.hpp"
#include "resource_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

constexpr std::size_t kMaximumColdMappedWindowBytes =
    current_address_space_window_limits().maximum_mapped_window_bytes;
constexpr std::size_t kColdMappedWindowAlignmentSlackBytes = 64U * 1024U;

struct ColdMappedWindowStats {
    std::size_t mapped_bytes{0U};
    std::size_t peak_mapped_bytes{0U};
    std::uint64_t mapping_hits{0U};
    std::uint64_t mapping_misses{0U};
    std::uint64_t remaps{0U};
    std::uint64_t touches{0U};
    std::uint64_t touched_bytes{0U};
    bool ledger_within_hard_limits{true};
    bool ledger_accounting_clean{true};
};

bool validate_cold_mapped_window_bytes(
    std::size_t window_bytes,
    std::string* error);

class ColdMappedWindow final {
public:
    explicit ColdMappedWindow(std::size_t window_bytes);
    ~ColdMappedWindow();

    ColdMappedWindow(const ColdMappedWindow&) = delete;
    ColdMappedWindow& operator=(const ColdMappedWindow&) = delete;
    ColdMappedWindow(ColdMappedWindow&&) = delete;
    ColdMappedWindow& operator=(ColdMappedWindow&&) = delete;

    bool touch(
        const std::filesystem::path& path,
        std::uint64_t offset,
        std::size_t bytes,
        std::string* error);

    void release() noexcept;
    ColdMappedWindowStats stats() const noexcept;
    std::size_t window_bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc
