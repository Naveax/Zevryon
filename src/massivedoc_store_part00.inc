#include "massivedoc_store.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <system_error>

namespace zevryon::massivedoc {
namespace {

constexpr std::array<char, 8> kManifestMagic{'Z', 'M', 'D', 'S', 'T', '0', '0', '1'};
constexpr std::array<char, 8> kCorpusMagic{'Z', 'M', 'D', 'O', 'C', '0', '0', '1'};
constexpr std::uint32_t kFormatVersion = 1U;
constexpr std::size_t kRecordDescriptorBytes = 32U;
constexpr std::size_t kChunkDescriptorBytes = 24U;
constexpr std::size_t kManifestBytes = 160U;

std::uint32_t crc32_update(std::uint32_t crc, std::span<const std::byte> bytes) {
    crc = ~crc;
    for (const std::byte raw : bytes) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<unsigned char>(raw));
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                  0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
        buffer_size_ = 0U;
        total_bytes_ = 0U;
    }

    void update(std::span<const std::byte> input) {
        if (input.empty()) {
            return;
        }
        total_bytes_ += static_cast<std::uint64_t>(input.size());
        std::size_t offset = 0U;
        if (buffer_size_ != 0U) {
            const std::size_t copy = std::min<std::size_t>(64U - buffer_size_, input.size());
            std::memcpy(buffer_.data() + buffer_size_, input.data(), copy);
            buffer_size_ += copy;
            offset += copy;
            if (buffer_size_ == 64U) {
                transform(buffer_.data());
                buffer_size_ = 0U;
            }
        }
        while (offset + 64U <= input.size()) {
            transform(reinterpret_cast<const std::uint8_t*>(input.data() + offset));
            offset += 64U;
        }
        const std::size_t remaining = input.size() - offset;
        if (remaining != 0U) {
            std::memcpy(buffer_.data(), input.data() + offset, remaining);
            buffer_size_ = remaining;
        }
    }

    std::array<std::uint8_t, 32> finish() const {
        Sha256 copy = *this;
        const std::uint64_t total_bits = copy.total_bytes_ * 8ULL;
        copy.buffer_[copy.buffer_size_++] = static_cast<std::uint8_t>(0x80U);
        if (copy.buffer_size_ > 56U) {
            std::fill(
                copy.buffer_.begin() + static_cast<std::ptrdiff_t>(copy.buffer_size_),
                copy.buffer_.end(),
                std::uint8_t{0});
            copy.transform(copy.buffer_.data());
            copy.buffer_size_ = 0U;
        }
        std::fill(
            copy.buffer_.begin() + static_cast<std::ptrdiff_t>(copy.buffer_size_),
            copy.buffer_.begin() + 56,
            std::uint8_t{0});
        for (std::size_t index = 0U; index < 8U; ++index) {
            copy.buffer_[56U + index] = static_cast<std::uint8_t>(total_bits >> ((7U - index) * 8U));
        }
        copy.transform(copy.buffer_.data());
        std::array<std::uint8_t, 32> output{};
        for (std::size_t index = 0U; index < copy.state_.size(); ++index) {
            const std::uint32_t value = copy.state_[index];
            output[index * 4U] = static_cast<std::uint8_t>(value >> 24U);
            output[index * 4U + 1U] = static_cast<std::uint8_t>(value >> 16U);
            output[index * 4U + 2U] = static_cast<std::uint8_t>(value >> 8U);
            output[index * 4U + 3U] = static_cast<std::uint8_t>(value);
        }
        return output;
    }

private:
    static constexpr std::array<std::uint32_t, 64> kRound{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    void transform(const std::uint8_t* block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                           (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                           static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const std::uint32_t s0 = std::rotr(words[index - 15U], 7) ^ std::rotr(words[index - 15U], 18) ^ (words[index - 15U] >> 3U);
            const std::uint32_t s1 = std::rotr(words[index - 2U], 17) ^ std::rotr(words[index - 2U], 19) ^ (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0U; index < kRound.size(); ++index) {
            const std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + sum1 + choose + kRound[index] + words[index];
            const std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
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

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_{0U};
    std::uint64_t total_bytes_{0U};
};
