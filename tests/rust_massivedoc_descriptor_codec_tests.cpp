#include "zevryon_massivedoc_rust_ffi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "rust MassiveDoc descriptor codec failure: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

template <typename Integer>
void write_le(std::uint8_t* output, Integer value) {
    static_assert(std::is_unsigned_v<Integer>);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

std::array<std::uint8_t, ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES> reference_record_bytes(
    const ZrMassiveDocRecordDescriptor& descriptor) {
    std::array<std::uint8_t, ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES> bytes{};
    write_le(bytes.data(), descriptor.logical_id);
    write_le(bytes.data() + 8U, descriptor.first_chunk);
    write_le(bytes.data() + 16U, descriptor.length);
    write_le(bytes.data() + 24U, descriptor.chunk_count);
    write_le(bytes.data() + 28U, descriptor.crc32);
    return bytes;
}

std::array<std::uint8_t, ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES> reference_chunk_bytes(
    const ZrMassiveDocChunkDescriptor& descriptor) {
    std::array<std::uint8_t, ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES> bytes{};
    write_le(bytes.data(), descriptor.segment_id);
    write_le(bytes.data() + 4U, std::uint32_t{0});
    write_le(bytes.data() + 8U, descriptor.offset);
    write_le(bytes.data() + 16U, descriptor.length);
    return bytes;
}

std::uint64_t next_random(std::uint64_t* state) {
    std::uint64_t value = *state;
    value ^= value << 13U;
    value ^= value >> 7U;
    value ^= value << 17U;
    *state = value;
    return value;
}

void check_known_layout() {
    require(zr_massivedoc_abi_version() == ZR_MASSIVEDOC_ABI_VERSION, "ABI version mismatch");
    require(
        zr_massivedoc_record_descriptor_size() == ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES,
        "record descriptor size mismatch");
    require(
        zr_massivedoc_chunk_descriptor_size() == ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES,
        "chunk descriptor size mismatch");

    const ZrMassiveDocRecordDescriptor record{
        0x0102030405060708ULL,
        0x1112131415161718ULL,
        0x2122232425262728ULL,
        0x31323334U,
        0x41424344U};
    std::array<std::uint8_t, ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES> encoded{};
    require(
        zr_massivedoc_encode_record_descriptor(&record, encoded.data(), encoded.size()) == 1U,
        "known record encode rejected");
    require(encoded == reference_record_bytes(record), "known record byte layout diverged");

    ZrMassiveDocRecordDescriptor decoded{};
    require(
        zr_massivedoc_decode_record_descriptor(encoded.data(), encoded.size(), &decoded) == 1U,
        "known record decode rejected");
    require(decoded.logical_id == record.logical_id, "record logical id diverged");
    require(decoded.first_chunk == record.first_chunk, "record first chunk diverged");
    require(decoded.length == record.length, "record length diverged");
    require(decoded.chunk_count == record.chunk_count, "record chunk count diverged");
    require(decoded.crc32 == record.crc32, "record CRC diverged");

    const ZrMassiveDocChunkDescriptor chunk{17U, 0xFFFFFFFFU, 65536U, 4096U};
    std::array<std::uint8_t, ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES> chunk_bytes{};
    require(
        zr_massivedoc_encode_chunk_descriptor(&chunk, chunk_bytes.data(), chunk_bytes.size()) == 1U,
        "known chunk encode rejected");
    require(chunk_bytes == reference_chunk_bytes(chunk), "known chunk byte layout diverged");
    require(
        chunk_bytes[4] == 0U && chunk_bytes[5] == 0U && chunk_bytes[6] == 0U &&
            chunk_bytes[7] == 0U,
        "chunk reserved bytes are not canonical zero");

    chunk_bytes[4] = 0xEFU;
    chunk_bytes[5] = 0xBEU;
    chunk_bytes[6] = 0xADU;
    chunk_bytes[7] = 0xDEU;
    ZrMassiveDocChunkDescriptor decoded_chunk{};
    require(
        zr_massivedoc_decode_chunk_descriptor(
            chunk_bytes.data(), chunk_bytes.size(), &decoded_chunk) == 1U,
        "chunk decode rejected compatible reserved bytes");
    require(decoded_chunk.segment_id == chunk.segment_id, "chunk segment diverged");
    require(decoded_chunk.reserved == 0U, "decoded reserved field is not canonical zero");
    require(decoded_chunk.offset == chunk.offset, "chunk offset diverged");
    require(decoded_chunk.length == chunk.length, "chunk length diverged");
}

void check_deterministic_oracle() {
    std::uint64_t state = 0xD1CEB00C5EED1234ULL;
    for (std::size_t iteration = 0; iteration < 100000U; ++iteration) {
        const ZrMassiveDocRecordDescriptor record{
            next_random(&state),
            next_random(&state),
            next_random(&state),
            static_cast<std::uint32_t>(next_random(&state)),
            static_cast<std::uint32_t>(next_random(&state))};
        std::array<std::uint8_t, ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES> encoded{};
        require(
            zr_massivedoc_encode_record_descriptor(&record, encoded.data(), encoded.size()) == 1U,
            "record encode rejected deterministic vector");
        require(encoded == reference_record_bytes(record), "record oracle bytes diverged");

        ZrMassiveDocRecordDescriptor decoded{};
        require(
            zr_massivedoc_decode_record_descriptor(encoded.data(), encoded.size(), &decoded) == 1U,
            "record decode rejected deterministic vector");
        require(decoded.logical_id == record.logical_id, "record oracle logical id diverged");
        require(decoded.first_chunk == record.first_chunk, "record oracle first chunk diverged");
        require(decoded.length == record.length, "record oracle length diverged");
        require(decoded.chunk_count == record.chunk_count, "record oracle chunk count diverged");
        require(decoded.crc32 == record.crc32, "record oracle CRC diverged");

        const ZrMassiveDocChunkDescriptor chunk{
            static_cast<std::uint32_t>(next_random(&state)),
            static_cast<std::uint32_t>(next_random(&state)),
            next_random(&state),
            next_random(&state)};
        std::array<std::uint8_t, ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES> chunk_bytes{};
        require(
            zr_massivedoc_encode_chunk_descriptor(&chunk, chunk_bytes.data(), chunk_bytes.size()) ==
                1U,
            "chunk encode rejected deterministic vector");
        require(chunk_bytes == reference_chunk_bytes(chunk), "chunk oracle bytes diverged");
    }
}

void check_checked_arithmetic() {
    constexpr std::uint64_t record_bytes = ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES;
    constexpr std::uint64_t chunk_bytes = ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES;
    std::uint64_t output = 0;

    require(zr_massivedoc_record_descriptor_offset(7U, &output) == 1U, "record offset rejected");
    require(output == 7U * record_bytes, "record offset diverged");
    const std::uint64_t max_record_index = std::numeric_limits<std::uint64_t>::max() / record_bytes;
    require(
        zr_massivedoc_record_descriptor_offset(max_record_index, &output) == 1U,
        "maximum valid record offset rejected");
    require(
        zr_massivedoc_record_descriptor_offset(max_record_index + 1U, &output) == 0U,
        "overflowing record offset accepted");

    require(zr_massivedoc_chunk_descriptor_offset(7U, &output) == 1U, "chunk offset rejected");
    require(output == 7U * chunk_bytes, "chunk offset diverged");
    const std::uint64_t max_chunk_index = std::numeric_limits<std::uint64_t>::max() / chunk_bytes;
    require(
        zr_massivedoc_chunk_descriptor_offset(max_chunk_index, &output) == 1U,
        "maximum valid chunk offset rejected");
    require(
        zr_massivedoc_chunk_descriptor_offset(max_chunk_index + 1U, &output) == 0U,
        "overflowing chunk offset accepted");

    ZrMassiveDocSlicePlan plan{};
    require(zr_massivedoc_plan_record_slice(100U, 25U, 40U, &plan) == 1U, "slice rejected");
    require(plan.offset == 25U && plan.length == 40U, "slice plan diverged");
    require(zr_massivedoc_plan_record_slice(100U, 90U, 40U, &plan) == 1U, "tail slice rejected");
    require(plan.offset == 90U && plan.length == 10U, "tail slice was not bounded");
    require(zr_massivedoc_plan_record_slice(100U, 101U, 1U, &plan) == 0U, "invalid slice accepted");

    require(
        zr_massivedoc_chunk_range_within_segment(1024U, 1000U, 24U) == 1U,
        "exact segment tail rejected");
    require(
        zr_massivedoc_chunk_range_within_segment(1024U, 1000U, 25U) == 0U,
        "segment overflow accepted");
    require(
        zr_massivedoc_chunk_range_within_segment(
            std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint64_t>::max(),
            0U) == 1U,
        "maximum empty tail rejected");
    require(
        zr_massivedoc_record_chunk_span_within_table(8U, 4U, 12U) == 1U,
        "valid chunk span rejected");
    require(
        zr_massivedoc_record_chunk_span_within_table(8U, 5U, 12U) == 0U,
        "out-of-table chunk span accepted");
    require(
        zr_massivedoc_record_chunk_span_within_table(
            std::numeric_limits<std::uint64_t>::max(), 1U,
            std::numeric_limits<std::uint64_t>::max()) == 0U,
        "overflowing chunk span accepted");
}

void check_fail_closed_ffi() {
    ZrMassiveDocRecordDescriptor record{};
    std::array<std::uint8_t, ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES> bytes{};
    require(
        zr_massivedoc_encode_record_descriptor(nullptr, bytes.data(), bytes.size()) == 0U,
        "null record pointer accepted");
    require(
        zr_massivedoc_encode_record_descriptor(&record, nullptr, bytes.size()) == 0U,
        "null record output accepted");
    require(
        zr_massivedoc_encode_record_descriptor(&record, bytes.data(), bytes.size() - 1U) == 0U,
        "short record output accepted");
    require(
        zr_massivedoc_decode_record_descriptor(nullptr, bytes.size(), &record) == 0U,
        "null record input accepted");
    require(
        zr_massivedoc_decode_record_descriptor(bytes.data(), bytes.size() - 1U, &record) == 0U,
        "short record input accepted");
    require(zr_massivedoc_record_descriptor_offset(0U, nullptr) == 0U, "null offset accepted");
    require(
        zr_massivedoc_plan_record_slice(0U, 0U, 0U, nullptr) == 0U,
        "null slice output accepted");
}

} // namespace

int main() {
    check_known_layout();
    check_deterministic_oracle();
    check_checked_arithmetic();
    check_fail_closed_ffi();
    std::cout << "Rust MassiveDoc descriptor codec oracle passed\n";
    return 0;
}
