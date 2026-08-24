#include "prefetch_record_bounds.hpp"

#include <algorithm>
#include <limits>

namespace zevryon::massivedoc {

RecordBoundPrefetchDecision clamp_prefetch_to_record(
    std::int8_t direction,
    std::uint64_t visible_edge_offset,
    std::uint64_t predicted_offset,
    std::size_t requested_bytes,
    std::uint64_t record_length) noexcept {
    RecordBoundPrefetchDecision result;
    if ((direction != 1 && direction != -1) || requested_bytes == 0U ||
        visible_edge_offset > record_length) {
        return result;
    }

    const std::uint64_t requested = static_cast<std::uint64_t>(requested_bytes);
    if (direction > 0) {
        if (visible_edge_offset == record_length) {
            result.eof_suppressed = true;
            return result;
        }
        if (predicted_offset < visible_edge_offset) {
            return result;
        }

        const bool predicted_fits =
            predicted_offset <= record_length &&
            requested <= record_length - predicted_offset;
        if (predicted_fits) {
            result.byte_offset = predicted_offset;
            result.request_bytes = requested_bytes;
            result.should_issue = true;
            return result;
        }

        if (record_length >= requested) {
            const std::uint64_t last_full_start = record_length - requested;
            if (last_full_start >= visible_edge_offset) {
                result.byte_offset = last_full_start;
                result.request_bytes = requested_bytes;
                result.should_issue = true;
                result.offset_clamped = last_full_start != predicted_offset;
                return result;
            }
        }

        const std::uint64_t tail = record_length - visible_edge_offset;
        if (tail == 0U || tail > static_cast<std::uint64_t>(
                                   std::numeric_limits<std::size_t>::max())) {
            result.eof_suppressed = tail == 0U;
            return result;
        }
        result.byte_offset = visible_edge_offset;
        result.request_bytes = static_cast<std::size_t>(tail);
        result.should_issue = true;
        result.offset_clamped = predicted_offset != visible_edge_offset;
        result.bytes_clamped = result.request_bytes != requested_bytes;
        return result;
    }

    if (visible_edge_offset == 0U || predicted_offset >= visible_edge_offset ||
        predicted_offset >= record_length) {
        result.eof_suppressed = visible_edge_offset == 0U;
        return result;
    }
    const std::uint64_t available_before_edge = visible_edge_offset - predicted_offset;
    const std::uint64_t exact = std::min<std::uint64_t>(requested, available_before_edge);
    if (exact == 0U ||
        exact > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return result;
    }
    result.byte_offset = predicted_offset;
    result.request_bytes = static_cast<std::size_t>(exact);
    result.should_issue = true;
    result.bytes_clamped = result.request_bytes != requested_bytes;
    return result;
}

} // namespace zevryon::massivedoc
