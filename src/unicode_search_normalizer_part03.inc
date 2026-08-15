bool UnicodeSearchNormalizer::process_source(
    const SearchSourceCodePoint& source,
    const UnicodeSearchNormalizationConsumer& consumer,
    UnicodeSearchNormalizationError* error) {
    if (source.value > kUnicodeMax || source.source_end < source.source_start) {
        return fail(
            UnicodeSearchNormalizationErrorKind::InvalidInput,
            source.source_start,
            "invalid decoded source code point",
            error);
    }
    ++stats_.input_codepoints;

    std::size_t first_decomposition_count = 0U;
    bool pipeline_ok = true;
    const auto append_final =
        [this, &source, &consumer, error, &pipeline_ok](std::uint32_t value) {
            const bool final_decomposition_ok = visit_decomposition(
                value,
                [this, &source, &consumer, error](std::uint32_t final_value) {
                    return append_decomposed(
                        final_value,
                        source.source_start,
                        source.source_end,
                        consumer,
                        error);
                },
                error,
                source.source_start);
            if (!final_decomposition_ok) {
                pipeline_ok = false;
                return false;
            }
            return true;
        };

    const bool decomposition_ok = visit_decomposition(
        source.value,
        [this,
         &source,
         error,
         &first_decomposition_count,
         &pipeline_ok,
         &append_final](std::uint32_t decomposed) {
            ++first_decomposition_count;
            if (!config_.full_case_fold) {
                if (!append_final(decomposed)) {
                    pipeline_ok = false;
                    return false;
                }
                return true;
            }
            std::size_t fold_count = 0U;
            const bool fold_ok = visit_case_fold(
                decomposed,
                [&append_final, &fold_count, &pipeline_ok](std::uint32_t folded_value) {
                    ++fold_count;
                    if (!append_final(folded_value)) {
                        pipeline_ok = false;
                        return false;
                    }
                    return true;
                },
                error,
                source.source_start);
            if (fold_count > 1U) {
                stats_.case_fold_expansions +=
                    static_cast<std::uint64_t>(fold_count - 1U);
            }
            if (!fold_ok) {
                pipeline_ok = false;
                return false;
            }
            return true;
        },
        error,
        source.source_start);

    if (first_decomposition_count > 1U) {
        stats_.canonical_decomposition_expansions +=
            static_cast<std::uint64_t>(first_decomposition_count - 1U);
    }
    if (!decomposition_ok || !pipeline_ok) {
        if (failed_) {
            return false;
        }
        return fail(
            error != nullptr ? error->kind : UnicodeSearchNormalizationErrorKind::InvalidTable,
            source.source_start,
            error != nullptr ? error->message : std::string_view{"normalization pipeline failed"},
            error);
    }
    return true;
}

bool UnicodeSearchNormalizer::feed(
    std::span<const SearchSourceCodePoint> input,
    const UnicodeSearchNormalizationConsumer& consumer,
    UnicodeSearchNormalizationError* error) {
    if (error != nullptr) {
        *error = {};
    }
    if (failed_) {
        return false;
    }
    if (finished_) {
        return fail(
            UnicodeSearchNormalizationErrorKind::AlreadyFinished,
            0U,
            "normalizer is already finished",
            error);
    }
    if (!tables_valid_) {
        return fail(
            UnicodeSearchNormalizationErrorKind::InvalidTable,
            0U,
            "normalization tables are invalid",
            error);
    }
    if (config_.max_pending_codepoints == 0U) {
        return fail(
            UnicodeSearchNormalizationErrorKind::InvalidConfiguration,
            0U,
            "max_pending_codepoints must be non-zero",
            error);
    }
    for (const auto& source : input) {
        if (!process_source(source, consumer, error)) {
            return false;
        }
    }
    return true;
}

bool UnicodeSearchNormalizer::finish(
    const UnicodeSearchNormalizationConsumer& consumer,
    UnicodeSearchNormalizationError* error) {
    if (error != nullptr) {
        *error = {};
    }
    if (failed_) {
        return false;
    }
    if (finished_) {
        return fail(
            UnicodeSearchNormalizationErrorKind::AlreadyFinished,
            0U,
            "normalizer is already finished",
            error);
    }
    if (!tables_valid_ || config_.max_pending_codepoints == 0U) {
        return fail(
            !tables_valid_ ? UnicodeSearchNormalizationErrorKind::InvalidTable
                           : UnicodeSearchNormalizationErrorKind::InvalidConfiguration,
            0U,
            !tables_valid_ ? "normalization tables are invalid"
                           : "max_pending_codepoints must be non-zero",
            error);
    }
    if (!flush_pending(consumer, error)) {
        return false;
    }
    finished_ = true;
    return true;
}

void UnicodeSearchNormalizer::reset() noexcept {
    stats_ = {};
    pending_.clear();
    emit_buffer_.clear();
    finished_ = false;
    failed_ = false;
}

const UnicodeSearchNormalizationStats& UnicodeSearchNormalizer::stats() const noexcept {
    return stats_;
}

bool UnicodeSearchNormalizer::finished() const noexcept {
    return finished_;
}

bool UnicodeSearchNormalizer::failed() const noexcept {
    return failed_;
}

} // namespace zevryon::text
