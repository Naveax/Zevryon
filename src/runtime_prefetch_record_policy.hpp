#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace zevryon::massivedoc {

class SharedRecordLengthAuthority;

enum class PrefetchRecordLengthLearnResult : std::uint8_t {
    NotApplicable = 0U,
    Learned,
    Failed,
};

struct RuntimePrefetchRecordPolicyDecision {
    std::uint64_t byte_offset{0U};
    std::size_t request_bytes{0U};
    bool metadata_hit{false};
    bool should_issue{true};
    bool clamped{false};
    bool eof_suppressed{false};
};

RuntimePrefetchRecordPolicyDecision apply_cached_record_bounds(
    SharedRecordLengthAuthority* authority,
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    std::int8_t direction,
    std::uint64_t visible_edge_offset,
    std::uint64_t predicted_offset,
    std::size_t requested_bytes) noexcept;

PrefetchRecordLengthLearnResult learn_record_length_from_short_prefetch(
    SharedRecordLengthAuthority* authority,
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    std::uint64_t byte_offset,
    std::size_t requested_bytes,
    std::size_t actual_bytes) noexcept;

} // namespace zevryon::massivedoc
