#pragma once

#include <cstddef>
#include <string_view>

namespace zevryon::massivedoc::detail {

// Return the byte width of one well-formed Unicode scalar encoded as UTF-8 at
// offset. Zero means malformed/truncated UTF-8. ASCII is a one-byte scalar.
// This deliberately validates only UTF-8 scalar structure; HTML input-stream
// preprocessing (NUL replacement and CR/LF normalization) is a separate layer.
inline std::size_t html_tokenizer_utf8_scalar_bytes_v1(
    std::string_view input,
    std::size_t offset) noexcept {
    if (offset >= input.size()) {
        return 0U;
    }

    const auto byte = [&input](std::size_t index) noexcept {
        return static_cast<unsigned char>(input[index]);
    };
    const auto continuation = [&byte, &input](std::size_t index) noexcept {
        return index < input.size() && (byte(index) & 0xC0U) == 0x80U;
    };

    const unsigned char first = byte(offset);
    if (first <= 0x7FU) {
        return 1U;
    }
    if (first >= 0xC2U && first <= 0xDFU) {
        return continuation(offset + 1U) ? 2U : 0U;
    }
    if (first == 0xE0U) {
        if (offset + 2U >= input.size()) {
            return 0U;
        }
        const unsigned char second = byte(offset + 1U);
        return second >= 0xA0U && second <= 0xBFU && continuation(offset + 2U)
            ? 3U : 0U;
    }
    if ((first >= 0xE1U && first <= 0xECU) ||
        (first >= 0xEEU && first <= 0xEFU)) {
        return continuation(offset + 1U) && continuation(offset + 2U) ? 3U : 0U;
    }
    if (first == 0xEDU) {
        if (offset + 2U >= input.size()) {
            return 0U;
        }
        const unsigned char second = byte(offset + 1U);
        return second >= 0x80U && second <= 0x9FU && continuation(offset + 2U)
            ? 3U : 0U;
    }
    if (first == 0xF0U) {
        if (offset + 3U >= input.size()) {
            return 0U;
        }
        const unsigned char second = byte(offset + 1U);
        return second >= 0x90U && second <= 0xBFU &&
                continuation(offset + 2U) && continuation(offset + 3U)
            ? 4U : 0U;
    }
    if (first >= 0xF1U && first <= 0xF3U) {
        return continuation(offset + 1U) && continuation(offset + 2U) &&
                continuation(offset + 3U)
            ? 4U : 0U;
    }
    if (first == 0xF4U) {
        if (offset + 3U >= input.size()) {
            return 0U;
        }
        const unsigned char second = byte(offset + 1U);
        return second >= 0x80U && second <= 0x8FU &&
                continuation(offset + 2U) && continuation(offset + 3U)
            ? 4U : 0U;
    }
    return 0U;
}

} // namespace zevryon::massivedoc::detail
