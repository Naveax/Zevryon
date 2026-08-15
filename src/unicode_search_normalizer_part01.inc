bool UnicodeSearchNormalizer::visit_decomposition(
    std::uint32_t codepoint,
    const std::function<bool(std::uint32_t)>& visitor,
    UnicodeSearchNormalizationError* error,
    std::uint64_t source_offset) const {
    std::array<std::uint32_t, 3> hangul{};
    std::size_t hangul_length = 0U;
    if (decompose_hangul(codepoint, hangul.data(), &hangul_length)) {
        for (std::size_t index = 0U; index < hangul_length; ++index) {
            if (!visitor(hangul[index])) {
                return false;
            }
        }
        return true;
    }

    const UnicodeSearchMapEntry* entry =
        find_map_entry(tables_.canonical_decomposition, codepoint);
    if (entry == nullptr) {
        return visitor(codepoint);
    }
    const std::size_t offset = static_cast<std::size_t>(entry->offset);
    const std::size_t length = static_cast<std::size_t>(entry->length);
    if (offset > tables_.canonical_decomposition_pool.size() ||
        length > tables_.canonical_decomposition_pool.size() - offset) {
        if (error != nullptr) {
            error->kind = UnicodeSearchNormalizationErrorKind::InvalidTable;
            error->source_offset = source_offset;
            error->message = "canonical decomposition table range is invalid";
        }
        return false;
    }
    for (std::size_t index = 0U; index < length; ++index) {
        if (!visitor(tables_.canonical_decomposition_pool[offset + index])) {
            return false;
        }
    }
    return true;
}

bool UnicodeSearchNormalizer::visit_case_fold(
    std::uint32_t codepoint,
    const std::function<bool(std::uint32_t)>& visitor,
    UnicodeSearchNormalizationError* error,
    std::uint64_t source_offset) const {
    const UnicodeSearchMapEntry* entry = find_map_entry(tables_.full_case_fold, codepoint);
    if (entry == nullptr) {
        return visitor(codepoint);
    }
    const std::size_t offset = static_cast<std::size_t>(entry->offset);
    const std::size_t length = static_cast<std::size_t>(entry->length);
    if (offset > tables_.full_case_fold_pool.size() ||
        length > tables_.full_case_fold_pool.size() - offset) {
        if (error != nullptr) {
            error->kind = UnicodeSearchNormalizationErrorKind::InvalidTable;
            error->source_offset = source_offset;
            error->message = "case-fold table range is invalid";
        }
        return false;
    }
    for (std::size_t index = 0U; index < length; ++index) {
        if (!visitor(tables_.full_case_fold_pool[offset + index])) {
            return false;
        }
    }
    return true;
}

bool UnicodeSearchNormalizer::append_pending(
    std::uint32_t value,
    std::uint64_t source_start,
    std::uint64_t source_end,
    std::uint8_t ccc,
    UnicodeSearchNormalizationError* error) {
    if (pending_.size() >= config_.max_pending_codepoints) {
        return fail(
            UnicodeSearchNormalizationErrorKind::PendingSequenceLimitExceeded,
            source_start,
            "normalization pending sequence exceeds configured bound",
            error);
    }
    pending_.push_back(PendingCodePoint{value, source_start, source_end, ccc});
    stats_.peak_pending_codepoints = std::max(stats_.peak_pending_codepoints, pending_.size());
    return true;
}


#include "unicode_search_normalizer_part02.inc"
