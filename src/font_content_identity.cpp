#include "font_content_identity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace zevryon::text {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::array<std::uint32_t, 8> kInitialState{
    0x6a09e667U,
    0xbb67ae85U,
    0x3c6ef372U,
    0xa54ff53aU,
    0x510e527fU,
    0x9b05688cU,
    0x1f83d9abU,
    0x5be0cd19U};

constexpr std::array<std::byte, 24> kFontIdentityDomain{
    std::byte{'z'}, std::byte{'e'}, std::byte{'v'}, std::byte{'r'},
    std::byte{'y'}, std::byte{'o'}, std::byte{'n'}, std::byte{'.'},
    std::byte{'f'}, std::byte{'o'}, std::byte{'n'}, std::byte{'t'},
    std::byte{'-'}, std::byte{'c'}, std::byte{'o'}, std::byte{'n'},
    std::byte{'t'}, std::byte{'e'}, std::byte{'n'}, std::byte{'t'},
    std::byte{'.'}, std::byte{'v'}, std::byte{'1'}, std::byte{0}};

constexpr std::uint64_t kMaximumMessageBytes =
    std::numeric_limits<std::uint64_t>::max() / 8U;

std::uint32_t rotate_right(std::uint32_t value, unsigned int shift) noexcept {
    return (value >> shift) | (value << (32U - shift));
}

std::uint32_t load_u32_be(const std::byte* bytes) noexcept {
    return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[0]))
            << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[1]))
            << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[2]))
            << 8U) |
           static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[3]));
}

void store_u32_be(std::uint32_t value, std::byte* output) noexcept {
    output[0] = std::byte{static_cast<unsigned char>(value >> 24U)};
    output[1] = std::byte{static_cast<unsigned char>(value >> 16U)};
    output[2] = std::byte{static_cast<unsigned char>(value >> 8U)};
    output[3] = std::byte{static_cast<unsigned char>(value)};
}

void store_u64_be(std::uint64_t value, std::byte* output) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
        const unsigned int shift =
            static_cast<unsigned int>((7U - index) * 8U);
        output[index] =
            std::byte{static_cast<unsigned char>(value >> shift)};
    }
}

std::uint64_t load_u64_be(const std::byte* bytes) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        value = (value << 8U) |
            static_cast<std::uint64_t>(
                std::to_integer<unsigned char>(bytes[index]));
    }
    return value;
}

} // namespace

Sha256::Sha256() noexcept {
    reset();
}

void Sha256::reset() noexcept {
    state_ = kInitialState;
    buffer_.fill(std::byte{0});
    total_bytes_ = 0U;
    buffered_bytes_ = 0U;
    finished_ = false;
}

bool Sha256::update(std::span<const std::byte> bytes) noexcept {
    if (finished_) {
        return false;
    }
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (bytes.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max())) {
            return false;
        }
    }
    const std::uint64_t incoming = static_cast<std::uint64_t>(bytes.size());
    if (incoming > kMaximumMessageBytes - total_bytes_) {
        return false;
    }
    total_bytes_ += incoming;

    std::size_t offset = 0U;
    if (buffered_bytes_ != 0U) {
        const std::size_t copied = std::min(
            bytes.size(), buffer_.size() - buffered_bytes_);
        if (copied != 0U) {
            std::memcpy(
                buffer_.data() + buffered_bytes_,
                bytes.data(),
                copied);
            buffered_bytes_ += copied;
            offset += copied;
        }
        if (buffered_bytes_ == buffer_.size()) {
            compress(buffer_.data());
            buffered_bytes_ = 0U;
        }
    }

    while (bytes.size() - offset >= buffer_.size()) {
        compress(bytes.data() + offset);
        offset += buffer_.size();
    }

    const std::size_t remaining = bytes.size() - offset;
    if (remaining != 0U) {
        std::memcpy(buffer_.data(), bytes.data() + offset, remaining);
        buffered_bytes_ = remaining;
    }
    return true;
}

bool Sha256::finish(Sha256Digest* output) noexcept {
    if (output == nullptr || finished_) {
        return false;
    }

    const std::uint64_t bit_length = total_bytes_ * 8U;
    buffer_[buffered_bytes_] = std::byte{0x80U};
    ++buffered_bytes_;

    if (buffered_bytes_ > 56U) {
        std::fill(
            buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_bytes_),
            buffer_.end(),
            std::byte{0});
        compress(buffer_.data());
        buffered_bytes_ = 0U;
    }

    std::fill(
        buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_bytes_),
        buffer_.begin() + 56,
        std::byte{0});
    store_u64_be(bit_length, buffer_.data() + 56U);
    compress(buffer_.data());

    for (std::size_t index = 0U; index < state_.size(); ++index) {
        store_u32_be(state_[index], output->data() + index * 4U);
    }
    finished_ = true;
    buffered_bytes_ = 0U;
    buffer_.fill(std::byte{0});
    return true;
}

std::uint64_t Sha256::total_bytes() const noexcept {
    return total_bytes_;
}

bool Sha256::finished() const noexcept {
    return finished_;
}

void Sha256::compress(const std::byte* block) noexcept {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0U; index < 16U; ++index) {
        words[index] = load_u32_be(block + index * 4U);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
        const std::uint32_t x = words[index - 15U];
        const std::uint32_t y = words[index - 2U];
        const std::uint32_t sigma0 =
            rotate_right(x, 7U) ^ rotate_right(x, 18U) ^ (x >> 3U);
        const std::uint32_t sigma1 =
            rotate_right(y, 17U) ^ rotate_right(y, 19U) ^ (y >> 10U);
        words[index] =
            words[index - 16U] + sigma0 + words[index - 7U] + sigma1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t index = 0U; index < words.size(); ++index) {
        const std::uint32_t sum1 =
            rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        const std::uint32_t choose = (e & f) ^ ((~e) & g);
        const std::uint32_t temporary1 =
            h + sum1 + choose + kRoundConstants[index] + words[index];
        const std::uint32_t sum0 =
            rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temporary2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

bool compute_font_content_identity(
    std::span<const std::byte> font_bytes,
    std::uint32_t face_index,
    FontContentIdentity* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    *output = {};

    std::array<std::byte, 4> face_bytes{};
    store_u32_be(face_index, face_bytes.data());
    std::array<std::byte, 8> length_bytes{};
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (font_bytes.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max())) {
            return false;
        }
    }
    store_u64_be(
        static_cast<std::uint64_t>(font_bytes.size()),
        length_bytes.data());

    Sha256 hash;
    Sha256Digest digest{};
    if (!hash.update(kFontIdentityDomain) ||
        !hash.update(face_bytes) ||
        !hash.update(length_bytes) ||
        !hash.update(font_bytes) ||
        !hash.finish(&digest)) {
        return false;
    }

    output->high = load_u64_be(digest.data());
    output->low = load_u64_be(digest.data() + 8U);
    output->face_index = face_index;
    if (output->high == 0U && output->low == 0U) {
        output->low = 1U;
    }
    return true;
}

} // namespace zevryon::text
