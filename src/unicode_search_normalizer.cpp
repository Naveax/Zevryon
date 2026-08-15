#include "unicode_search_normalizer.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace zevryon::text {
namespace {

constexpr std::uint32_t kUnicodeMax = 0x10ffffU;
constexpr std::uint32_t kHangulSBase = 0xac00U;
constexpr std::uint32_t kHangulLBase = 0x1100U;
constexpr std::uint32_t kHangulVBase = 0x1161U;
constexpr std::uint32_t kHangulTBase = 0x11a7U;
constexpr std::uint32_t kHangulLCount = 19U;
constexpr std::uint32_t kHangulVCount = 21U;
constexpr std::uint32_t kHangulTCount = 28U;
constexpr std::uint32_t kHangulNCount = kHangulVCount * kHangulTCount;
constexpr std::uint32_t kHangulSCount = kHangulLCount * kHangulNCount;

bool valid_map_entries(
    std::span<const UnicodeSearchMapEntry> entries,
    std::span<const std::uint32_t> pool) noexcept {
    std::uint32_t previous = 0U;
    bool have_previous = false;
    for (const auto& entry : entries) {
        if (entry.codepoint > kUnicodeMax ||
            (have_previous && entry.codepoint <= previous)) {
            return false;
        }
        const std::size_t offset = static_cast<std::size_t>(entry.offset);
        const std::size_t length = static_cast<std::size_t>(entry.length);
        if (offset > pool.size() || length > pool.size() - offset) {
            return false;
        }
        for (std::size_t index = 0U; index < length; ++index) {
            if (pool[offset + index] > kUnicodeMax) {
                return false;
            }
        }
        previous = entry.codepoint;
        have_previous = true;
    }
    return true;
}

} // namespace

bool validate_unicode_search_normalization_tables(
    const UnicodeSearchNormalizationTables& tables) noexcept {
    if (tables.unicode_version.empty() || tables.fingerprint.empty() ||
        !valid_map_entries(
            tables.canonical_decomposition,
            tables.canonical_decomposition_pool) ||
        !valid_map_entries(tables.full_case_fold, tables.full_case_fold_pool)) {
        return false;
    }

    std::uint32_t previous_last = 0U;
    bool have_previous = false;
    for (const auto& range : tables.combining_classes) {
        if (range.first > range.last || range.last > kUnicodeMax ||
            range.combining_class == 0U ||
            (have_previous && range.first <= previous_last)) {
            return false;
        }
        previous_last = range.last;
        have_previous = true;
    }

    std::uint64_t previous_key = 0U;
    have_previous = false;
    for (const auto& entry : tables.compositions) {
        if (entry.first > kUnicodeMax || entry.second > kUnicodeMax ||
            entry.composite > kUnicodeMax) {
            return false;
        }
        const std::uint64_t key =
            (static_cast<std::uint64_t>(entry.first) << 21U) |
            static_cast<std::uint64_t>(entry.second);
        if (have_previous && key <= previous_key) {
            return false;
        }
        previous_key = key;
        have_previous = true;
    }
    return true;
}

UnicodeSearchNormalizer::UnicodeSearchNormalizer(
    const UnicodeSearchNormalizationTables& tables,
    UnicodeSearchNormalizerConfig config)
    : tables_(tables), config_(config),
      tables_valid_(validate_unicode_search_normalization_tables(tables)) {
    if (config_.max_pending_codepoints != 0U) {
        pending_.reserve(config_.max_pending_codepoints);
        emit_buffer_.reserve(config_.max_pending_codepoints);
    }
}

bool UnicodeSearchNormalizer::fail(
    UnicodeSearchNormalizationErrorKind kind,
    std::uint64_t source_offset,
    std::string_view message,
    UnicodeSearchNormalizationError* error) noexcept {
    failed_ = true;
    if (error != nullptr) {
        error->kind = kind;
        error->source_offset = source_offset;
        error->message = message;
    }
    return false;
}

const UnicodeSearchMapEntry* UnicodeSearchNormalizer::find_map_entry(
    std::span<const UnicodeSearchMapEntry> entries,
    std::uint32_t codepoint) const noexcept {
    const auto iterator = std::lower_bound(
        entries.begin(), entries.end(), codepoint,
        [](const UnicodeSearchMapEntry& entry, std::uint32_t value) {
            return entry.codepoint < value;
        });
    return iterator != entries.end() && iterator->codepoint == codepoint
        ? &*iterator
        : nullptr;
}

std::uint8_t UnicodeSearchNormalizer::combining_class(
    std::uint32_t codepoint) const noexcept {
    const auto ranges = tables_.combining_classes;
    std::size_t first = 0U;
    std::size_t count = ranges.size();
    while (count != 0U) {
        const std::size_t step = count / 2U;
        const std::size_t middle = first + step;
        if (ranges[middle].last < codepoint) {
            first = middle + 1U;
            count -= step + 1U;
        } else {
            count = step;
        }
    }
    if (first < ranges.size() &&
        ranges[first].first <= codepoint && codepoint <= ranges[first].last) {
        return ranges[first].combining_class;
    }
    return 0U;
}

std::uint32_t UnicodeSearchNormalizer::compose_pair(
    std::uint32_t first,
    std::uint32_t second) const noexcept {
    const std::uint32_t hangul = compose_hangul(first, second);
    if (hangul != 0U) {
        return hangul;
    }
    const std::uint64_t wanted =
        (static_cast<std::uint64_t>(first) << 21U) |
        static_cast<std::uint64_t>(second);
    const auto entries = tables_.compositions;
    const auto iterator = std::lower_bound(
        entries.begin(), entries.end(), wanted,
        [](const UnicodeSearchCompositionEntry& entry, std::uint64_t key) {
            const std::uint64_t entry_key =
                (static_cast<std::uint64_t>(entry.first) << 21U) |
                static_cast<std::uint64_t>(entry.second);
            return entry_key < key;
        });
    return iterator != entries.end() && iterator->first == first &&
            iterator->second == second
        ? iterator->composite
        : 0U;
}

bool UnicodeSearchNormalizer::decompose_hangul(
    std::uint32_t codepoint,
    std::uint32_t* output,
    std::size_t* length) noexcept {
    if (codepoint < kHangulSBase || codepoint >= kHangulSBase + kHangulSCount) {
        return false;
    }
    const std::uint32_t s_index = codepoint - kHangulSBase;
    output[0] = kHangulLBase + s_index / kHangulNCount;
    output[1] = kHangulVBase + (s_index % kHangulNCount) / kHangulTCount;
    const std::uint32_t t_index = s_index % kHangulTCount;
    if (t_index != 0U) {
        output[2] = kHangulTBase + t_index;
        *length = 3U;
    } else {
        *length = 2U;
    }
    return true;
}

std::uint32_t UnicodeSearchNormalizer::compose_hangul(
    std::uint32_t first,
    std::uint32_t second) noexcept {
    if (first >= kHangulLBase && first < kHangulLBase + kHangulLCount &&
        second >= kHangulVBase && second < kHangulVBase + kHangulVCount) {
        const std::uint32_t l_index = first - kHangulLBase;
        const std::uint32_t v_index = second - kHangulVBase;
        return kHangulSBase + (l_index * kHangulVCount + v_index) * kHangulTCount;
    }
    if (first >= kHangulSBase && first < kHangulSBase + kHangulSCount &&
        (first - kHangulSBase) % kHangulTCount == 0U &&
        second > kHangulTBase && second < kHangulTBase + kHangulTCount) {
        return first + (second - kHangulTBase);
    }
    return 0U;
}

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

bool UnicodeSearchNormalizer::append_decomposed(
    std::uint32_t value,
    std::uint64_t source_start,
    std::uint64_t source_end,
    const UnicodeSearchNormalizationConsumer& consumer,
    UnicodeSearchNormalizationError* error) {
    const std::uint8_t ccc = combining_class(value);
    if (ccc == 0U && !pending_.empty()) {
        if (pending_.size() == 1U && pending_[0].combining_class == 0U) {
            const std::uint32_t composite = compose_hangul(pending_[0].value, value);
            if (composite != 0U) {
                pending_[0].value = composite;
                pending_[0].source_start = std::min(pending_[0].source_start, source_start);
                pending_[0].source_end = std::max(pending_[0].source_end, source_end);
                return true;
            }
        }
        if (!flush_pending(consumer, error)) {
            return false;
        }
    }
    return append_pending(value, source_start, source_end, ccc, error);
}

bool UnicodeSearchNormalizer::flush_pending(
    const UnicodeSearchNormalizationConsumer& consumer,
    UnicodeSearchNormalizationError* error) {
    if (pending_.empty()) {
        return true;
    }

    const std::size_t sort_start = pending_[0].combining_class == 0U ? 1U : 0U;
    std::stable_sort(
        pending_.begin() + static_cast<std::ptrdiff_t>(sort_start),
        pending_.end(),
        [](const PendingCodePoint& left, const PendingCodePoint& right) {
            return left.combining_class < right.combining_class;
        });

    emit_buffer_.clear();
    if (pending_[0].combining_class != 0U) {
        for (const auto& item : pending_) {
            emit_buffer_.push_back(
                NormalizedSearchCodePoint{item.value, item.source_start, item.source_end});
        }
    } else {
        PendingCodePoint starter = pending_[0];
        emit_buffer_.push_back(
            NormalizedSearchCodePoint{starter.value, starter.source_start, starter.source_end});
        std::size_t starter_index = 0U;
        std::uint8_t last_ccc = 0U;
        for (std::size_t index = 1U; index < pending_.size(); ++index) {
            const PendingCodePoint& current = pending_[index];
            const std::uint32_t composite =
                compose_pair(emit_buffer_[starter_index].value, current.value);
            if (composite != 0U && (last_ccc == 0U || last_ccc < current.combining_class)) {
                auto& target = emit_buffer_[starter_index];
                target.value = composite;
                target.source_start = std::min(target.source_start, current.source_start);
                target.source_end = std::max(target.source_end, current.source_end);
            } else {
                emit_buffer_.push_back(NormalizedSearchCodePoint{
                    current.value,
                    current.source_start,
                    current.source_end});
                last_ccc = current.combining_class;
            }
        }
    }

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
    const bool decomposition_ok = visit_decomposition(
        source.value,
        [this,
         &source,
         &consumer,
         error,
         &first_decomposition_count,
         &pipeline_ok](std::uint32_t decomposed) {
            ++first_decomposition_count;
            std::size_t fold_count = 0U;
            const bool fold_ok = visit_case_fold(
                decomposed,
                [this,
                 &source,
                 &consumer,
                 error,
                 &fold_count,
                 &pipeline_ok](std::uint32_t folded_value) {
                    ++fold_count;
                    const bool final_decomposition_ok = visit_decomposition(
                        folded_value,
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
