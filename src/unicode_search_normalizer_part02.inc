bool UnicodeSearchNormalizer::append_decomposed(
    std::uint32_t value,
    std::uint64_t source_start,
    std::uint64_t source_end,
    const UnicodeSearchNormalizationConsumer& consumer,
    UnicodeSearchNormalizationError* error) {
    const std::uint8_t ccc = combining_class(value);
    if (ccc == 0U && !pending_.empty()) {
        if (config_.compose && pending_[0].combining_class == 0U) {
            prepare_pending_output();
            if (emit_buffer_.size() == 1U) {
                const std::uint32_t composite =
                    compose_pair(emit_buffer_[0].value, value);
                if (composite != 0U) {
                    const std::uint64_t combined_start =
                        std::min(emit_buffer_[0].source_start, source_start);
                    const std::uint64_t combined_end =
                        std::max(emit_buffer_[0].source_end, source_end);
                    pending_.clear();
                    pending_.push_back(
                        PendingCodePoint{composite, combined_start, combined_end, 0U});
                    return true;
                }
            }
        }
        if (!flush_pending(consumer, error)) {
            return false;
        }
    }
    return append_pending(value, source_start, source_end, ccc, error);
}

void UnicodeSearchNormalizer::prepare_pending_output() {
    const std::size_t sort_start = pending_[0].combining_class == 0U ? 1U : 0U;
    std::stable_sort(
        pending_.begin() + static_cast<std::ptrdiff_t>(sort_start),
        pending_.end(),
        [](const PendingCodePoint& left, const PendingCodePoint& right) {
            return left.combining_class < right.combining_class;
        });

    emit_buffer_.clear();
    if (!config_.compose || pending_[0].combining_class != 0U) {
        for (const auto& item : pending_) {
            emit_buffer_.push_back(
                NormalizedSearchCodePoint{item.value, item.source_start, item.source_end});
        }
        return;
    }

    emit_buffer_.push_back(NormalizedSearchCodePoint{
        pending_[0].value, pending_[0].source_start, pending_[0].source_end});
    std::size_t starter_index = 0U;
    std::uint8_t last_ccc = 0U;
    for (std::size_t index = 1U; index < pending_.size(); ++index) {
        const PendingCodePoint& current = pending_[index];
        const std::uint32_t composite =
            compose_pair(emit_buffer_[starter_index].value, current.value);
        if (composite != 0U &&
            (last_ccc == 0U || last_ccc < current.combining_class)) {
            auto& target = emit_buffer_[starter_index];
            target.value = composite;
            target.source_start = std::min(target.source_start, current.source_start);
            target.source_end = std::max(target.source_end, current.source_end);
        } else {
            emit_buffer_.push_back(NormalizedSearchCodePoint{
                current.value, current.source_start, current.source_end});
            last_ccc = current.combining_class;
        }
    }
}

bool UnicodeSearchNormalizer::flush_pending(
    const UnicodeSearchNormalizationConsumer& consumer,
    UnicodeSearchNormalizationError* error) {
    if (pending_.empty()) {
        return true;
    }

    prepare_pending_output();

    if (!consumer(std::span<const NormalizedSearchCodePoint>(emit_buffer_))) {
        return fail(
            UnicodeSearchNormalizationErrorKind::ConsumerStopped,
            emit_buffer_.empty() ? 0U : emit_buffer_.front().source_start,
            "normalization consumer stopped",
            error);
    }
    stats_.emitted_codepoints += static_cast<std::uint64_t>(emit_buffer_.size());
    ++stats_.emitted_segments;
    pending_.clear();
    return true;
}


#include "unicode_search_normalizer_part03.inc"
