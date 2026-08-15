#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace zevryon::massivedoc::detail {

struct UnicodeSearchRuntimeLimits {
    std::size_t max_query_bytes{64U * 1024U};
    std::size_t max_query_codepoints{4096U};
    std::size_t max_pending_codepoints{256U};
};

struct UnicodeSearchRuntimeStats {
    std::uint64_t source_bytes_decoded{0U};
    std::uint64_t normalized_codepoints{0U};
};

class UnicodeSearchRuntime {
public:
    explicit UnicodeSearchRuntime(UnicodeSearchRuntimeLimits limits) noexcept;
    ~UnicodeSearchRuntime();

    UnicodeSearchRuntime(const UnicodeSearchRuntime&) = delete;
    UnicodeSearchRuntime& operator=(const UnicodeSearchRuntime&) = delete;

    bool build_pattern(std::string_view query_utf8, std::string* error);
    std::size_t pattern_codepoints() const noexcept;

    void reset_record() noexcept;
    bool feed_record(
        std::span<const std::byte> bytes,
        std::uint64_t absolute_offset,
        std::string* error);
    bool finish_record(std::string* error);

    bool found() const noexcept;
    std::uint64_t match_source_start() const noexcept;
    std::uint64_t match_source_end() const noexcept;
    UnicodeSearchRuntimeStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc::detail
