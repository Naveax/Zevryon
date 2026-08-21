#include "prefetch_tail_admission.hpp"

namespace zevryon::massivedoc {

PrefetchTailAdmissionResult canonicalize_prefetch_tail_for_exact_admission(
    SourceWindowPrefetchResult* result) noexcept {
    if (result == nullptr || !result->succeeded || result->request.max_bytes == 0U ||
        result->bytes.empty() || result->bytes.size() > result->request.max_bytes) {
        return PrefetchTailAdmissionResult::Invalid;
    }
    if (result->bytes.size() == result->request.max_bytes) {
        return PrefetchTailAdmissionResult::Unchanged;
    }
    result->request.max_bytes = result->bytes.size();
    return PrefetchTailAdmissionResult::Canonicalized;
}

} // namespace zevryon::massivedoc
