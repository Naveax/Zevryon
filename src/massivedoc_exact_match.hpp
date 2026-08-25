#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace zevryon::massivedoc {

enum class ExactByteMatchBackend : std::uint8_t {
    Scalar = 0U,
    Sse2,
    Neon,
};

struct ExactByteMatchStats {
    std::uint64_t simd_batches{0U};
    std::uint64_t scalar_candidates{0U};
    std::uint64_t exact_compares{0U};
};

ExactByteMatchBackend exact_byte_match_backend() noexcept;
const char* exact_byte_match_backend_name(ExactByteMatchBackend backend) noexcept;
bool exact_byte_match_simd_available() noexcept;
inline constexpr std::size_t kExactByteMatchNotFound = static_cast<std::size_t>(-1);

std::size_t find_exact_bytes_scalar(
    std::span<const std::byte> haystack,
    std::span<const std::byte> needle,
    ExactByteMatchStats* stats = nullptr) noexcept;

std::size_t find_exact_bytes(
    std::span<const std::byte> haystack,
    std::span<const std::byte> needle,
    ExactByteMatchStats* stats = nullptr) noexcept;

} // namespace zevryon::massivedoc
