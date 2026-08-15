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


#include "unicode_search_normalizer_part01.inc"
