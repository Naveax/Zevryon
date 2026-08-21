#include "runtime_prefetch_record_policy.hpp"

#include "prefetch_record_bounds.hpp"
#include "shared_record_length_authority.hpp"

#include <limits>
#include <string>

namespace zevryon::massivedoc {

RuntimePrefetchRecordPolicyDecision apply_cached_record_bounds(
    SharedRecordLengthAuthority* authority,
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    std::int8_t direction,
    std::uint64_t visible_edge_offset,
    std::uint64_t predicted_offset,
    std::size_t requested_bytes) noexcept {
    RuntimePrefetchRecordPolicyDecision result;
    result.byte_offset = predicted_offset;
    result.request_bytes = requested_bytes;
    if (authority == nullptr || requested_bytes == 0U) {
        return result;
    }

    std::uint64_t record_length = 0U;
    try {
        if (!authority->try_get(store_root, record_index, &record_length)) {
            return result;
        }
    } catch (...) {
        return result;
    }
    result.metadata_hit = true;
    const RecordBoundPrefetchDecision bounded = clamp_prefetch_to_record(
        direction,
        visible_edge_offset,
        predicted_offset,
        requested_bytes,
        record_length);
    result.should_issue = bounded.should_issue;
    result.eof_suppressed = bounded.eof_suppressed;
    if (!bounded.should_issue) {
        result.request_bytes = 0U;
        return result;
    }
    result.byte_offset = bounded.byte_offset;
    result.request_bytes = bounded.request_bytes;
    result.clamped = bounded.offset_clamped || bounded.bytes_clamped;
    return result;
}

PrefetchRecordLengthLearnResult learn_record_length_from_short_prefetch(
    SharedRecordLengthAuthority* authority,
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    std::uint64_t byte_offset,
    std::size_t requested_bytes,
    std::size_t actual_bytes) noexcept {
    if (authority == nullptr || actual_bytes == 0U || requested_bytes == 0U ||
        actual_bytes >= requested_bytes) {
        return PrefetchRecordLengthLearnResult::NotApplicable;
    }
    if (actual_bytes > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()) ||
        byte_offset > std::numeric_limits<std::uint64_t>::max() -
                          static_cast<std::uint64_t>(actual_bytes)) {
        return PrefetchRecordLengthLearnResult::Failed;
    }
    const std::uint64_t record_length =
        byte_offset + static_cast<std::uint64_t>(actual_bytes);
    std::string error;
    try {
        return authority->remember(store_root, record_index, record_length, &error)
                   ? PrefetchRecordLengthLearnResult::Learned
                   : PrefetchRecordLengthLearnResult::Failed;
    } catch (...) {
        return PrefetchRecordLengthLearnResult::Failed;
    }
}

} // namespace zevryon::massivedoc
