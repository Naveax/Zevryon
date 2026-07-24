#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace zevryon::text {

using Sha256Digest = std::array<std::byte, 32>;

class Sha256 final {
public:
    Sha256() noexcept;

    void reset() noexcept;
    bool update(std::span<const std::byte> bytes) noexcept;
    bool finish(Sha256Digest* output) noexcept;

    std::uint64_t total_bytes() const noexcept;
    bool finished() const noexcept;

private:
    void compress(const std::byte* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<std::byte, 64> buffer_{};
    std::uint64_t total_bytes_{0};
    std::size_t buffered_bytes_{0};
    bool finished_{false};
};

struct FontContentIdentity {
    std::uint64_t high{0};
    std::uint64_t low{0};
    std::uint32_t face_index{0};

    bool operator==(const FontContentIdentity&) const noexcept = default;
};

static_assert(
    sizeof(FontContentIdentity) <= 24U,
    "font content identities must remain compact");

// Hashes the domain marker, selected face index, exact byte length, and font
// bytes with SHA-256, then publishes the first 128 digest bits as the portable
// cache identity. The all-zero 128-bit value is deterministically remapped to
// low=1 so it remains valid for the cache key contract.
bool compute_font_content_identity(
    std::span<const std::byte> font_bytes,
    std::uint32_t face_index,
    FontContentIdentity* output) noexcept;

} // namespace zevryon::text
