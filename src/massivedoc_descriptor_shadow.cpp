#include "massivedoc_descriptor_shadow.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
#include "zevryon_massivedoc_rust_ffi.h"
#endif

namespace zevryon::massivedoc {
namespace {

std::atomic<std::uint64_t> g_record_encode_checks{0};
std::atomic<std::uint64_t> g_record_decode_checks{0};
std::atomic<std::uint64_t> g_chunk_encode_checks{0};
std::atomic<std::uint64_t> g_chunk_decode_checks{0};
std::atomic<std::uint64_t> g_mismatches{0};
std::atomic<std::uint32_t> g_first_mismatch{
    static_cast<std::uint32_t>(MassiveDocDescriptorShadowOperation::None)};

void record_mismatch(MassiveDocDescriptorShadowOperation operation) noexcept {
    g_mismatches.fetch_add(1U, std::memory_order_relaxed);
    std::uint32_t expected =
        static_cast<std::uint32_t>(MassiveDocDescriptorShadowOperation::None);
    (void)g_first_mismatch.compare_exchange_strong(
        expected,
        static_cast<std::uint32_t>(operation),
        std::memory_order_relaxed,
        std::memory_order_relaxed);
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW_STRICT) && \
    ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW_STRICT
    std::abort();
#endif
}

#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
const std::uint8_t* byte_pointer(std::span<const std::byte> bytes) noexcept {
    return reinterpret_cast<const std::uint8_t*>(bytes.data());
}
#endif

} // namespace

void reset_massivedoc_descriptor_shadow() noexcept {
    g_record_encode_checks.store(0U, std::memory_order_relaxed);
    g_record_decode_checks.store(0U, std::memory_order_relaxed);
    g_chunk_encode_checks.store(0U, std::memory_order_relaxed);
    g_chunk_decode_checks.store(0U, std::memory_order_relaxed);
    g_mismatches.store(0U, std::memory_order_relaxed);
    g_first_mismatch.store(
        static_cast<std::uint32_t>(MassiveDocDescriptorShadowOperation::None),
        std::memory_order_relaxed);
}

MassiveDocDescriptorShadowSnapshot massivedoc_descriptor_shadow_snapshot() noexcept {
    MassiveDocDescriptorShadowSnapshot snapshot;
    snapshot.record_encode_checks =
        g_record_encode_checks.load(std::memory_order_relaxed);
    snapshot.record_decode_checks =
        g_record_decode_checks.load(std::memory_order_relaxed);
    snapshot.chunk_encode_checks =
        g_chunk_encode_checks.load(std::memory_order_relaxed);
    snapshot.chunk_decode_checks =
        g_chunk_decode_checks.load(std::memory_order_relaxed);
    snapshot.mismatches = g_mismatches.load(std::memory_order_relaxed);
    snapshot.first_mismatch = static_cast<MassiveDocDescriptorShadowOperation>(
        g_first_mismatch.load(std::memory_order_relaxed));
    return snapshot;
}

void verify_massivedoc_record_encoding(
    std::uint64_t logical_id,
    std::uint64_t first_chunk,
    std::uint64_t length,
    std::uint32_t chunk_count,
    std::uint32_t crc32,
    std::span<const std::byte> cpp_bytes) noexcept {
    g_record_encode_checks.fetch_add(1U, std::memory_order_relaxed);
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
    const ZrMassiveDocRecordDescriptor descriptor{
        logical_id, first_chunk, length, chunk_count, crc32};
    std::array<std::uint8_t, ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES> rust_bytes{};
    const bool encoded = zr_massivedoc_encode_record_descriptor(
                             &descriptor, rust_bytes.data(), rust_bytes.size()) != 0U;
    const bool equal = cpp_bytes.size() == rust_bytes.size() &&
                       std::memcmp(
                           byte_pointer(cpp_bytes), rust_bytes.data(), rust_bytes.size()) == 0;
    if (!encoded || !equal) {
        record_mismatch(MassiveDocDescriptorShadowOperation::RecordEncode);
    }
#else
    (void)logical_id;
    (void)first_chunk;
    (void)length;
    (void)chunk_count;
    (void)crc32;
    (void)cpp_bytes;
#endif
}

void verify_massivedoc_record_decoding(
    std::span<const std::byte> encoded,
    std::uint64_t logical_id,
    std::uint64_t first_chunk,
    std::uint64_t length,
    std::uint32_t chunk_count,
    std::uint32_t crc32) noexcept {
    g_record_decode_checks.fetch_add(1U, std::memory_order_relaxed);
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
    ZrMassiveDocRecordDescriptor rust{};
    const bool decoded = zr_massivedoc_decode_record_descriptor(
                             byte_pointer(encoded), encoded.size(), &rust) != 0U;
    const bool equal = decoded && rust.logical_id == logical_id &&
                       rust.first_chunk == first_chunk && rust.length == length &&
                       rust.chunk_count == chunk_count && rust.crc32 == crc32;
    if (!equal) {
        record_mismatch(MassiveDocDescriptorShadowOperation::RecordDecode);
    }
#else
    (void)encoded;
    (void)logical_id;
    (void)first_chunk;
    (void)length;
    (void)chunk_count;
    (void)crc32;
#endif
}

void verify_massivedoc_chunk_encoding(
    std::uint32_t segment_id,
    std::uint64_t offset,
    std::uint64_t length,
    std::span<const std::byte> cpp_bytes) noexcept {
    g_chunk_encode_checks.fetch_add(1U, std::memory_order_relaxed);
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
    const ZrMassiveDocChunkDescriptor descriptor{segment_id, 0U, offset, length};
    std::array<std::uint8_t, ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES> rust_bytes{};
    const bool encoded = zr_massivedoc_encode_chunk_descriptor(
                             &descriptor, rust_bytes.data(), rust_bytes.size()) != 0U;
    const bool equal = cpp_bytes.size() == rust_bytes.size() &&
                       std::memcmp(
                           byte_pointer(cpp_bytes), rust_bytes.data(), rust_bytes.size()) == 0;
    if (!encoded || !equal) {
        record_mismatch(MassiveDocDescriptorShadowOperation::ChunkEncode);
    }
#else
    (void)segment_id;
    (void)offset;
    (void)length;
    (void)cpp_bytes;
#endif
}

void verify_massivedoc_chunk_decoding(
    std::span<const std::byte> encoded,
    std::uint32_t segment_id,
    std::uint64_t offset,
    std::uint64_t length) noexcept {
    g_chunk_decode_checks.fetch_add(1U, std::memory_order_relaxed);
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
    ZrMassiveDocChunkDescriptor rust{};
    const bool decoded = zr_massivedoc_decode_chunk_descriptor(
                             byte_pointer(encoded), encoded.size(), &rust) != 0U;
    const bool equal = decoded && rust.segment_id == segment_id && rust.reserved == 0U &&
                       rust.offset == offset && rust.length == length;
    if (!equal) {
        record_mismatch(MassiveDocDescriptorShadowOperation::ChunkDecode);
    }
#else
    (void)encoded;
    (void)segment_id;
    (void)offset;
    (void)length;
#endif
}

} // namespace zevryon::massivedoc
