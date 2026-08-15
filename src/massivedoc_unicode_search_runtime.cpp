#include "massivedoc_unicode_search_runtime.hpp"

#include "massivedoc_unicode_matcher.hpp"
#include "unicode_search_normalization_data.generated.hpp"
#include "unicode_search_normalizer.hpp"
#include "unicode_stream.hpp"

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <new>
#include <vector>

namespace zevryon::massivedoc::detail {
namespace {
constexpr std::size_t kDecodeBatchBytes = 4096U;
constexpr std::size_t kDecodeArenaBytes = 80U * 1024U;

std::string utf8_error_message(const text::Utf8DecodeError& error) {
    std::string message = "invalid UTF-8 at byte " + std::to_string(error.source_offset);
    if (!error.message.empty()) {
        message += ": " + error.message;
    }
    return message;
}

std::string normalization_error_message(
    const text::UnicodeSearchNormalizationError& error) {
    std::string message = "Unicode search normalization failed at byte " +
        std::to_string(error.source_offset);
    if (!error.message.empty()) {
        message += ": ";
        message.append(error.message.data(), error.message.size());
    }
    return message;
}
} // namespace

struct UnicodeSearchRuntime::Impl {
    explicit Impl(UnicodeSearchRuntimeLimits input_limits)
        : limits(input_limits),
          arena(kDecodeArenaBytes),
          arena_resource(
              arena.data(),
              arena.size(),
              std::pmr::null_memory_resource()),
          decoded(&arena_resource),
          normalizer(
              text::kUnicodeSearchNormalizationTables,
              text::UnicodeSearchNormalizerConfig{
                  input_limits.max_pending_codepoints,
                  true,
                  true}) {
        decoded.reserve(kDecodeBatchBytes + 1U);
        source_codepoints.reserve(kDecodeBatchBytes + 1U);
    }

    void reset_pipeline() noexcept {
        decoder.reset();
        normalizer.reset();
        decoded.clear();
        source_codepoints.clear();
    }

    bool normalize_decoded_query(
        std::vector<text::NormalizedSearchCodePoint>* normalized,
        bool* limit_hit,
        std::string* error) {
        source_codepoints.clear();
        source_codepoints.reserve(decoded.size());
        for (const auto& value : decoded) {
            source_codepoints.push_back(text::SearchSourceCodePoint{
                value.value,
                value.source_start,
                value.source_end()});
        }
        text::UnicodeSearchNormalizationError normalization_error;
        const bool ok = normalizer.feed(
            source_codepoints,
            [this, normalized, limit_hit](
                std::span<const text::NormalizedSearchCodePoint> values) {
                if (values.size() > limits.max_query_codepoints - normalized->size()) {
                    *limit_hit = true;
                    return false;
                }
                normalized->insert(normalized->end(), values.begin(), values.end());
                return true;
            },
            &normalization_error);
        if (!ok && !*limit_hit) {
            *error = normalization_error_message(normalization_error);
        }
        return ok;
    }

    bool normalize_decoded_record(std::string* error) {
        source_codepoints.clear();
        source_codepoints.reserve(decoded.size());
        for (const auto& value : decoded) {
            source_codepoints.push_back(text::SearchSourceCodePoint{
                value.value,
                value.source_start,
                value.source_end()});
        }
        text::UnicodeSearchNormalizationError normalization_error;
        const bool ok = normalizer.feed(
            source_codepoints,
            [this](std::span<const text::NormalizedSearchCodePoint> values) {
                if (matcher != nullptr) {
                    static_cast<void>(matcher->feed(values));
                }
                return true;
            },
            &normalization_error);
        if (!ok) {
            *error = normalization_error_message(normalization_error);
        }
        return ok;
    }

    UnicodeSearchRuntimeLimits limits;
    std::vector<std::byte> arena;
    std::pmr::monotonic_buffer_resource arena_resource;
    std::pmr::vector<text::DecodedCodePoint> decoded;
    std::vector<text::SearchSourceCodePoint> source_codepoints;
    text::Utf8StreamDecoder decoder{text::Utf8ErrorPolicy::Strict};
    text::UnicodeSearchNormalizer normalizer;
    UnicodeSearchPattern pattern;
    std::unique_ptr<UnicodeSearchMatcher> matcher;
};

UnicodeSearchRuntime::UnicodeSearchRuntime(UnicodeSearchRuntimeLimits limits) noexcept {
    try {
        impl_ = std::make_unique<Impl>(limits);
    } catch (...) {
        impl_.reset();
    }
}

UnicodeSearchRuntime::~UnicodeSearchRuntime() = default;

bool UnicodeSearchRuntime::build_pattern(
    std::string_view query_utf8,
    std::string* error) {
    error->clear();
    if (impl_ == nullptr) {
        *error = "cannot allocate bounded Unicode search runtime";
        return false;
    }
    if (impl_->limits.max_query_bytes == 0U ||
        impl_->limits.max_query_codepoints == 0U ||
        impl_->limits.max_pending_codepoints == 0U) {
        *error = "Unicode search limits must be non-zero";
        return false;
    }
    if (query_utf8.size() > impl_->limits.max_query_bytes) {
        *error = "Unicode search query exceeds configured byte bound";
        return false;
    }
    if (query_utf8.empty()) {
        *error = "Unicode search query is empty";
        return false;
    }

    impl_->reset_pipeline();
    std::vector<text::NormalizedSearchCodePoint> normalized;
    normalized.reserve(std::min<std::size_t>(
        query_utf8.size(),
        impl_->limits.max_query_codepoints));
    bool limit_hit = false;
    std::uint64_t absolute_offset = 0U;
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(query_utf8.data()),
        query_utf8.size());
    std::size_t position = 0U;
    while (position < bytes.size()) {
        const std::size_t amount = std::min(kDecodeBatchBytes, bytes.size() - position);
        impl_->decoded.clear();
        text::Utf8DecodeError decode_error;
        if (!impl_->decoder.feed(
                bytes.subspan(position, amount),
                absolute_offset,
                &impl_->decoded,
                &decode_error)) {
            *error = utf8_error_message(decode_error);
            return false;
        }
        if (!impl_->normalize_decoded_query(&normalized, &limit_hit, error)) {
            if (limit_hit) {
                *error = "normalized Unicode search query exceeds configured codepoint bound";
            }
            return false;
        }
        position += amount;
        absolute_offset += static_cast<std::uint64_t>(amount);
    }

    impl_->decoded.clear();
    text::Utf8DecodeError decode_error;
    if (!impl_->decoder.finish(&impl_->decoded, &decode_error)) {
        *error = utf8_error_message(decode_error);
        return false;
    }
    if (!impl_->normalize_decoded_query(&normalized, &limit_hit, error)) {
        if (limit_hit) {
            *error = "normalized Unicode search query exceeds configured codepoint bound";
        }
        return false;
    }
    text::UnicodeSearchNormalizationError normalization_error;
    if (!impl_->normalizer.finish(
            [impl = impl_.get(), &normalized, &limit_hit](
                std::span<const text::NormalizedSearchCodePoint> values) {
                if (values.size() > impl->limits.max_query_codepoints - normalized.size()) {
                    limit_hit = true;
                    return false;
                }
                normalized.insert(normalized.end(), values.begin(), values.end());
                return true;
            },
            &normalization_error)) {
        if (limit_hit) {
            *error = "normalized Unicode search query exceeds configured codepoint bound";
        } else {
            *error = normalization_error_message(normalization_error);
        }
        return false;
    }

    UnicodeSearchPatternError pattern_error;
    if (!build_unicode_search_pattern(
            normalized,
            UnicodeSearchPatternConfig{impl_->limits.max_query_codepoints},
            &impl_->pattern,
            &pattern_error)) {
        *error = pattern_error.message.empty()
            ? "cannot build normalized Unicode search pattern"
            : std::string(pattern_error.message);
        return false;
    }
    try {
        impl_->matcher = std::make_unique<UnicodeSearchMatcher>(impl_->pattern);
    } catch (...) {
        *error = "cannot allocate normalized Unicode search matcher";
        return false;
    }
    reset_record();
    return true;
}

std::size_t UnicodeSearchRuntime::pattern_codepoints() const noexcept {
    return impl_ != nullptr ? impl_->pattern.codepoints.size() : 0U;
}

void UnicodeSearchRuntime::reset_record() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->reset_pipeline();
    if (impl_->matcher != nullptr) {
        impl_->matcher->reset();
    }
}

bool UnicodeSearchRuntime::feed_record(
    std::span<const std::byte> bytes,
    std::uint64_t absolute_offset,
    std::string* error) {
    error->clear();
    if (impl_ == nullptr || impl_->matcher == nullptr) {
        *error = "Unicode search pattern is not initialized";
        return false;
    }
    std::size_t position = 0U;
    while (position < bytes.size() && !impl_->matcher->match().found) {
        const std::size_t amount = std::min(kDecodeBatchBytes, bytes.size() - position);
        impl_->decoded.clear();
        text::Utf8DecodeError decode_error;
        if (!impl_->decoder.feed(
                bytes.subspan(position, amount),
                absolute_offset + static_cast<std::uint64_t>(position),
                &impl_->decoded,
                &decode_error)) {
            *error = utf8_error_message(decode_error);
            return false;
        }
        if (!impl_->normalize_decoded_record(error)) {
            return false;
        }
        position += amount;
    }
    return true;
}

bool UnicodeSearchRuntime::finish_record(std::string* error) {
    error->clear();
    if (impl_ == nullptr || impl_->matcher == nullptr) {
        *error = "Unicode search pattern is not initialized";
        return false;
    }
    if (impl_->matcher->match().found) {
        return true;
    }
    impl_->decoded.clear();
    text::Utf8DecodeError decode_error;
    if (!impl_->decoder.finish(&impl_->decoded, &decode_error)) {
        *error = utf8_error_message(decode_error);
        return false;
    }
    if (!impl_->normalize_decoded_record(error)) {
        return false;
    }
    if (impl_->matcher->match().found) {
        return true;
    }
    text::UnicodeSearchNormalizationError normalization_error;
    if (!impl_->normalizer.finish(
            [impl = impl_.get()](std::span<const text::NormalizedSearchCodePoint> values) {
                static_cast<void>(impl->matcher->feed(values));
                return true;
            },
            &normalization_error)) {
        *error = normalization_error_message(normalization_error);
        return false;
    }
    return true;
}

bool UnicodeSearchRuntime::found() const noexcept {
    return impl_ != nullptr && impl_->matcher != nullptr && impl_->matcher->match().found;
}

std::uint64_t UnicodeSearchRuntime::match_source_start() const noexcept {
    return found() ? impl_->matcher->match().source_start : 0U;
}

std::uint64_t UnicodeSearchRuntime::match_source_end() const noexcept {
    return found() ? impl_->matcher->match().source_end : 0U;
}

UnicodeSearchRuntimeStats UnicodeSearchRuntime::stats() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    return UnicodeSearchRuntimeStats{
        impl_->decoder.stats().source_bytes,
        impl_->normalizer.stats().emitted_codepoints};
}

} // namespace zevryon::massivedoc::detail
