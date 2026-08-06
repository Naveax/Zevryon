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

#if defined(ZEVRYON_MASSIVEDOC_CODEC_AUTHORITY_TEST_HOOKS)
std::atomic<std::uint32_t> g_cpp_reverse_shadow_fault{
    static_cast<std::uint32_t>(MassiveDocDescriptorShadowOperation::None)};
#endif

#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
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

const std::uint8_t* byte_pointer(std::span<const std::byte> bytes) noexcept {
    return reinterpret_cast<const std::uint8_t*>(bytes.data());
}

bool rust_contract_valid() noexcept {
    return zr_massivedoc_abi_version() == ZR_MASSIVEDOC_ABI_VERSION &&
           zr_massivedoc_record_descriptor_size() ==
               ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES &&
           zr_massivedoc_chunk_descriptor_size() ==
               ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES;
}

bool bytes_equal(
    std::span<const std::byte> cpp_bytes,
    const std::uint8_t* rust_bytes,
    std::size_t rust_size) noexcept {
    return cpp_bytes.size() == rust_size &&
           std::memcmp(byte_pointer(cpp_bytes), rust_bytes, rust_size) == 0;
}

#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE)
bool cpp_reverse_shadow_fault_enabled(
    MassiveDocDescriptorShadowOperation operation) noexcept {
#if defined(ZEVRYON_MASSIVEDOC_CODEC_AUTHORITY_TEST_HOOKS)
    return g_cpp_reverse_shadow_fault.load(std::memory_order_relaxed) ==
           static_cast<std::uint32_t>(operation);
#else
    (void)operation;
    return false;
#endif
}

[[noreturn]] void abort_authority() noexcept {
    std::abort();
}
#endif
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
#if defined(ZEVRYON_MASSIVEDOC_CODEC_AUTHORITY_TEST_HOOKS)
    g_cpp_reverse_shadow_fault.store(
        static_cast<std::uint32_t>(MassiveDocDescriptorShadowOperation::None),
        std::memory_order_relaxed);
#endif
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
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE)
    snapshot.authoritative_backend = MassiveDocDescriptorBackend::Rust;
    snapshot.verification_backend = MassiveDocDescriptorBackend::Cpp;
#else
    snapshot.authoritative_backend = MassiveDocDescriptorBackend::Cpp;
    snapshot.verification_backend = MassiveDocDescriptorBackend::Rust;
#endif
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
    if (!rust_contract_valid()) {
        record_mismatch(MassiveDocDescriptorShadowOperation::RecordEncode);
        return;
    }
    const ZrMassiveDocRecordDescriptor descriptor{
        logical_id, first_chunk, length, chunk_count, crc32};
    std::array<std::uint8_t, ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES> rust_bytes{};
    const bool encoded = zr_massivedoc_encode_record_descriptor(
                             &descriptor, rust_bytes.data(), rust_bytes.size()) != 0U;
    const bool equal = encoded &&
                       bytes_equal(cpp_bytes, rust_bytes.data(), rust_bytes.size());
    if (!equal) {
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
    if (!rust_contract_valid()) {
        record_mismatch(MassiveDocDescriptorShadowOperation::RecordDecode);
        return;
    }
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
    if (!rust_contract_valid()) {
        record_mismatch(MassiveDocDescriptorShadowOperation::ChunkEncode);
        return;
    }
    const ZrMassiveDocChunkDescriptor descriptor{segment_id, 0U, offset, length};
    std::array<std::uint8_t, ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES> rust_bytes{};
    const bool encoded = zr_massivedoc_encode_chunk_descriptor(
                             &descriptor, rust_bytes.data(), rust_bytes.size()) != 0U;
    const bool equal = encoded &&
                       bytes_equal(cpp_bytes, rust_bytes.data(), rust_bytes.size());
    if (!equal) {
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
    if (!rust_contract_valid()) {
        record_mismatch(MassiveDocDescriptorShadowOperation::ChunkDecode);
        return;
    }
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

#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE)
std::array<std::byte, kMassiveDocRecordDescriptorBytes>
encode_massivedoc_record_authoritative(
    const MassiveDocRecordDescriptorValue& descriptor,
    std::span<const std::byte> cpp_reverse_shadow_bytes) noexcept {
    g_record_encode_checks.fetch_add(1U, std::memory_order_relaxed);
    if (!rust_contract_valid()) {
        record_mismatch(MassiveDocDescriptorShadowOperation::RecordEncode);
        abort_authority();
    }

    const ZrMassiveDocRecordDescriptor rust_descriptor{
        descriptor.logical_id,
        descriptor.first_chunk,
        descriptor.length,
        descriptor.chunk_count,
        descriptor.crc32};
    std::array<std::uint8_t, ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES> rust_bytes{};
    const bool encoded = zr_massivedoc_encode_record_descriptor(
                             &rust_descriptor,
                             rust_bytes.data(),
                             rust_bytes.size()) != 0U;
    bool equal = encoded && bytes_equal(
                                cpp_reverse_shadow_bytes,
                                rust_bytes.data(),
                                rust_bytes.size());
    if (cpp_reverse_shadow_fault_enabled(
            MassiveDocDescriptorShadowOperation::RecordEncode)) {
        equal = false;
    }
    if (!encoded || !equal) {
        record_mismatch(MassiveDocDescriptorShadowOperation::RecordEncode);
    }
    if (!encoded) {
        abort_authority();
    }

    std::array<std::byte, kMassiveDocRecordDescriptorBytes> output{};
    std::memcpy(output.data(), rust_bytes.data(), output.size());
    return output;
}

std::optional<MassiveDocRecordDescriptorValue>
decode_massivedoc_record_authoritative(
    std::span<const std::byte> encoded,
    const std::optional<MassiveDocRecordDescriptorValue>& cpp_reverse_shadow) noexcept {
    g_record_decode_checks.fetch_add(1U, std::memory_order_relaxed);
    if (!rust_contract_valid()) {
        record_mismatch(MassiveDocDescriptorShadowOperation::RecordDecode);
        abort_authority();
    }

    ZrMassiveDocRecordDescriptor rust{};
    const bool decoded = zr_massivedoc_decode_record_descriptor(
                             byte_pointer(encoded), encoded.size(), &rust) != 0U;
    std::optional<MassiveDocRecordDescriptorValue> rust_value;
    if (decoded) {
        rust_value = MassiveDocRecordDescriptorValue{
            rust.logical_id,
            rust.first_chunk,
            rust.length,
            rust.chunk_count,
            rust.crc32};
    }

    bool equal = rust_value == cpp_reverse_shadow;
    if (cpp_reverse_shadow_fault_enabled(
            MassiveDocDescriptorShadowOperation::RecordDecode)) {
        equal = false;
    }
    if (!equal) {
        record_mismatch(MassiveDocDescriptorShadowOperation::RecordDecode);
    }
    return rust_value;
}

std::array<std::byte, kMassiveDocChunkDescriptorBytes>
encode_massivedoc_chunk_authoritative(
    const MassiveDocChunkDescriptorValue& descriptor,
    std::span<const std::byte> cpp_reverse_shadow_bytes) noexcept {
    g_chunk_encode_checks.fetch_add(1U, std::memory_order_relaxed);
    if (!rust_contract_valid()) {
        record_mismatch(MassiveDocDescriptorShadowOperation::ChunkEncode);
        abort_authority();
    }

    const ZrMassiveDocChunkDescriptor rust_descriptor{
        descriptor.segment_id, 0U, descriptor.offset, descriptor.length};
    std::array<std::uint8_t, ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES> rust_bytes{};
    const bool encoded = zr_massivedoc_encode_chunk_descriptor(
                             &rust_descriptor,
                             rust_bytes.data(),
                             rust_bytes.size()) != 0U;
    bool equal = encoded && bytes_equal(
                                cpp_reverse_shadow_bytes,
                                rust_bytes.data(),
                                rust_bytes.size());
    if (cpp_reverse_shadow_fault_enabled(
            MassiveDocDescriptorShadowOperation::ChunkEncode)) {
        equal = false;
    }
    if (!encoded || !equal) {
        record_mismatch(MassiveDocDescriptorShadowOperation::ChunkEncode);
    }
    if (!encoded) {
        abort_authority();
    }

    std::array<std::byte, kMassiveDocChunkDescriptorBytes> output{};
    std::memcpy(output.data(), rust_bytes.data(), output.size());
    return output;
}

std::optional<MassiveDocChunkDescriptorValue>
decode_massivedoc_chunk_authoritative(
    std::span<const std::byte> encoded,
    const std::optional<MassiveDocChunkDescriptorValue>& cpp_reverse_shadow) noexcept {
    g_chunk_decode_checks.fetch_add(1U, std::memory_order_relaxed);
    if (!rust_contract_valid()) {
        record_mismatch(MassiveDocDescriptorShadowOperation::ChunkDecode);
        abort_authority();
    }

    ZrMassiveDocChunkDescriptor rust{};
    const bool decoded = zr_massivedoc_decode_chunk_descriptor(
                             byte_pointer(encoded), encoded.size(), &rust) != 0U;
    std::optional<MassiveDocChunkDescriptorValue> rust_value;
    if (decoded) {
        rust_value = MassiveDocChunkDescriptorValue{
            rust.segment_id, rust.offset, rust.length};
    }

    bool equal = rust_value == cpp_reverse_shadow;
    if (cpp_reverse_shadow_fault_enabled(
            MassiveDocDescriptorShadowOperation::ChunkDecode)) {
        equal = false;
    }
    if (!equal) {
        record_mismatch(MassiveDocDescriptorShadowOperation::ChunkDecode);
    }
    return rust_value;
}
#endif

#if defined(ZEVRYON_MASSIVEDOC_CODEC_AUTHORITY_TEST_HOOKS)
void set_massivedoc_cpp_reverse_shadow_fault(
    MassiveDocDescriptorShadowOperation operation) noexcept {
    g_cpp_reverse_shadow_fault.store(
        static_cast<std::uint32_t>(operation), std::memory_order_relaxed);
}

void clear_massivedoc_cpp_reverse_shadow_fault() noexcept {
    g_cpp_reverse_shadow_fault.store(
        static_cast<std::uint32_t>(MassiveDocDescriptorShadowOperation::None),
        std::memory_order_relaxed);
}
#endif

} // namespace zevryon::massivedoc
