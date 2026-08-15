#include "massivedoc_exact_match.hpp"

#include <array>
#include <cstring>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define ZEVRYON_EXACT_MATCH_SSE2 1
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define ZEVRYON_EXACT_MATCH_NEON 1
#endif

namespace zevryon::massivedoc {
namespace {
inline bool exact_at(
    const std::byte* haystack,
    const std::byte* needle,
    std::size_t size,
    ExactByteMatchStats* stats) noexcept {
    if (stats != nullptr) {
        ++stats->exact_compares;
    }
    return std::memcmp(haystack, needle, size) == 0;
}
inline unsigned byte_value(std::byte value) noexcept {
    return static_cast<unsigned>(std::to_integer<unsigned char>(value));
}
}

bool exact_byte_match_simd_available() noexcept {
#if defined(ZEVRYON_EXACT_MATCH_SSE2) || defined(ZEVRYON_EXACT_MATCH_NEON)
    return true;
#else
    return false;
#endif
}

std::size_t find_exact_bytes(
    std::span<const std::byte> haystack,
    std::span<const std::byte> needle,
    ExactByteMatchStats* stats) noexcept {
    if (stats != nullptr) {
        *stats = {};
    }
    if (needle.empty()) {
        return 0U;
    }
    if (needle.size() > haystack.size()) {
        return kExactByteMatchNotFound;
    }
    if (needle.size() == 1U) {
        const unsigned wanted = byte_value(needle[0]);
        for (std::size_t index = 0U; index < haystack.size(); ++index) {
            if (stats != nullptr) {
                ++stats->scalar_candidates;
            }
            if (byte_value(haystack[index]) == wanted) {
                return index;
            }
        }
        return kExactByteMatchNotFound;
    }

    const std::size_t last_start = haystack.size() - needle.size();
    std::size_t offset = 0U;
    const unsigned first = byte_value(needle.front());
    const unsigned last = byte_value(needle.back());

#if defined(ZEVRYON_EXACT_MATCH_SSE2)
    const __m128i first_vector = _mm_set1_epi8(static_cast<char>(first));
    const __m128i last_vector = _mm_set1_epi8(static_cast<char>(last));
    while (offset + 15U <= last_start) {
        const auto* begin = reinterpret_cast<const __m128i*>(haystack.data() + offset);
        const auto* end = reinterpret_cast<const __m128i*>(
            haystack.data() + offset + needle.size() - 1U);
        const __m128i begin_bytes = _mm_loadu_si128(begin);
        const __m128i end_bytes = _mm_loadu_si128(end);
        unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(
            _mm_and_si128(
                _mm_cmpeq_epi8(begin_bytes, first_vector),
                _mm_cmpeq_epi8(end_bytes, last_vector))));
        if (stats != nullptr) {
            ++stats->simd_batches;
        }
        while (mask != 0U) {
            unsigned lane = 0U;
#if defined(_MSC_VER)
            unsigned long bit_index = 0UL;
            _BitScanForward(&bit_index, mask);
            lane = static_cast<unsigned>(bit_index);
#else
            lane = static_cast<unsigned>(__builtin_ctz(mask));
#endif
            const std::size_t candidate = offset + static_cast<std::size_t>(lane);
            if (exact_at(
                    haystack.data() + candidate,
                    needle.data(),
                    needle.size(),
                    stats)) {
                return candidate;
            }
            mask &= mask - 1U;
        }
        offset += 16U;
    }
#elif defined(ZEVRYON_EXACT_MATCH_NEON)
    const uint8x16_t first_vector = vdupq_n_u8(static_cast<std::uint8_t>(first));
    const uint8x16_t last_vector = vdupq_n_u8(static_cast<std::uint8_t>(last));
    while (offset + 15U <= last_start) {
        const uint8x16_t begin_bytes = vld1q_u8(
            reinterpret_cast<const std::uint8_t*>(haystack.data() + offset));
        const uint8x16_t end_bytes = vld1q_u8(
            reinterpret_cast<const std::uint8_t*>(
                haystack.data() + offset + needle.size() - 1U));
        const uint8x16_t both = vandq_u8(
            vceqq_u8(begin_bytes, first_vector),
            vceqq_u8(end_bytes, last_vector));
        if (stats != nullptr) {
            ++stats->simd_batches;
        }
        std::array<std::uint8_t, 16> lanes{};
        vst1q_u8(lanes.data(), both);
        for (std::size_t lane = 0U; lane < lanes.size(); ++lane) {
            if (lanes[lane] == 0U) {
                continue;
            }
            const std::size_t candidate = offset + lane;
            if (exact_at(
                    haystack.data() + candidate,
                    needle.data(),
                    needle.size(),
                    stats)) {
                return candidate;
            }
        }
        offset += 16U;
    }
#endif

    for (; offset <= last_start; ++offset) {
        if (stats != nullptr) {
            ++stats->scalar_candidates;
        }
        if (byte_value(haystack[offset]) != first ||
            byte_value(haystack[offset + needle.size() - 1U]) != last) {
            continue;
        }
        if (exact_at(
                haystack.data() + offset,
                needle.data(),
                needle.size(),
                stats)) {
            return offset;
        }
    }
    return kExactByteMatchNotFound;
}

} // namespace zevryon::massivedoc
