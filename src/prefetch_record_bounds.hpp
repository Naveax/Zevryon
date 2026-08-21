#pragma once

#include <cstddef>
#include <cstdint>

namespace zevryon::massivedoc {

struct RecordBoundPrefetchDecision {
    std::uint64_t byte_offset{0U};
    std::size_t request_bytes{0U};
    bool should_issue{false};
    bool offset_clamped{false};
    bool bytes_clamped{false};
    bool eof_suppressed{false};
};

RecordBoundPrefetchDecision clamp_prefetch_to_record(
    std::int8_t direction,
    std::uint64_t visible_edge_offset,
    std::uint64_t predicted_offset,
    std::size_t requested_bytes,
    std::uint64_t record_length) noexcept;

} // namespace zevryon::massivedoc
