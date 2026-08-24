#pragma once

#include "hot_scroll_source_prefetch.hpp"

#include <cstdint>

namespace zevryon::massivedoc {

enum class PrefetchTailAdmissionResult : std::uint8_t {
    Unchanged = 0U,
    Canonicalized,
    Invalid,
};

PrefetchTailAdmissionResult canonicalize_prefetch_tail_for_exact_admission(
    SourceWindowPrefetchResult* result) noexcept;

} // namespace zevryon::massivedoc
