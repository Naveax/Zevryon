#include "zevryon_massivedoc_rust_ffi.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

using RecordBytes = std::array<std::uint8_t, ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES>;
using ChunkBytes = std::array<std::uint8_t, ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES>;

void store_u64_le(std::uint8_t* output, std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
        output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

void store_u32_le(std::uint8_t* output, std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

RecordBytes encode_record_cpp(const ZrMassiveDocRecordDescriptor& descriptor) {
    RecordBytes output{};
    store_u64_le(output.data(), descriptor.logical_id);
    store_u64_le(output.data() + 8U, descriptor.first_chunk);
    store_u64_le(output.data() + 16U, descriptor.length);
    store_u32_le(output.data() + 24U, descriptor.chunk_count);
    store_u32_le(output.data() + 28U, descriptor.crc32);
    return output;
}

ChunkBytes encode_chunk_cpp(const ZrMassiveDocChunkDescriptor& descriptor) {
    ChunkBytes output{};
    store_u32_le(output.data(), descriptor.segment_id);
    store_u32_le(output.data() + 4U, 0U);
    store_u64_le(output.data() + 8U, descriptor.offset);
    store_u64_le(output.data() + 16U, descriptor.length);
    return output;
}

void assert_record_equal(
    const ZrMassiveDocRecordDescriptor& left,
    const ZrMassiveDocRecordDescriptor& right) {
    assert(left.logical_id == right.logical_id);
    assert(left.first_chunk == right.first_chunk);
    assert(left.length == right.length);
    assert(left.chunk_count == right.chunk_count);
    assert(left.crc32 == right.crc32);
}

void assert_chunk_equal(
    const ZrMassiveDocChunkDescriptor& left,
    const ZrMassiveDocChunkDescriptor& right) {
    assert(left.segment_id == right.segment_id);
    assert(left.offset == right.offset);
    assert(left.length == right.length);
}

} // namespace

int main() {
    assert(zr_massivedoc_abi_version() == ZR_MASSIVEDOC_ABI_VERSION);
    assert(zr_massivedoc_record_descriptor_size() == ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES);
    assert(zr_massivedoc_chunk_descriptor_size() == ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES);

    const std::array<ZrMassiveDocRecordDescriptor, 4> records{{
        {},
        {1U, 2U, 3U, 4U, 5U},
        {0x0102'0304'0506'0708ULL, 0x1112'1314'1516'1718ULL,
         0x2122'2324'2526'2728ULL, 0x3132'3334U, 0x4142'4344U},
        {std::numeric_limits<std::uint64_t>::max(),
         std::numeric_limits<std::uint64_t>::max(),
         std::numeric_limits<std::uint64_t>::max(),
         std::numeric_limits<std::uint32_t>::max(),
         std::numeric_limits<std::uint32_t>::max()},
    }};

    for (const auto& record : records) {
        const RecordBytes expected = encode_record_cpp(record);
        RecordBytes actual{};
        assert(zr_massivedoc_encode_record_descriptor(
                   &record,
                   actual.data(),
                   actual.size()) == 1U);
        assert(actual == expected);

        ZrMassiveDocRecordDescriptor decoded{};
        assert(zr_massivedoc_decode_record_descriptor(
                   actual.data(),
                   actual.size(),
                   &decoded) == 1U);
        assert_record_equal(decoded, record);
    }

    RecordBytes record_bytes{};
    const ZrMassiveDocRecordDescriptor sample_record{9U, 11U, 4096U, 3U, 0x1234'5678U};
    assert(zr_massivedoc_encode_record_descriptor(
               &sample_record,
               record_bytes.data(),
               record_bytes.size() - 1U) == 0U);
    assert(zr_massivedoc_decode_record_descriptor(
               record_bytes.data(),
               record_bytes.size() - 1U,
               nullptr) == 0U);

    const std::array<ZrMassiveDocChunkDescriptor, 3> chunks{{
        {},
        {7U, 0xFFFF'FFFFU, 64U * 1024U, 4096U},
        {std::numeric_limits<std::uint32_t>::max(), 0U,
         std::numeric_limits<std::uint64_t>::max(),
         std::numeric_limits<std::uint64_t>::max()},
    }};

    for (const auto& chunk : chunks) {
        const ChunkBytes expected = encode_chunk_cpp(chunk);
        ChunkBytes actual{};
        assert(zr_massivedoc_encode_chunk_descriptor(
                   &chunk,
                   actual.data(),
                   actual.size()) == 1U);
        assert(actual == expected);
        assert(actual[4] == 0U && actual[5] == 0U && actual[6] == 0U && actual[7] == 0U);

        actual[4] = 0xEFU;
        actual[5] = 0xBEU;
        actual[6] = 0xADU;
        actual[7] = 0xDEU;
        ZrMassiveDocChunkDescriptor decoded{};
        assert(zr_massivedoc_decode_chunk_descriptor(
                   actual.data(),
                   actual.size(),
                   &decoded) == 1U);
        assert(decoded.reserved == 0U);
        assert_chunk_equal(decoded, chunk);
    }

    std::uint64_t offset = 0U;
    assert(zr_massivedoc_record_descriptor_offset(7U, &offset) == 1U);
    assert(offset == 224U);
    assert(zr_massivedoc_chunk_descriptor_offset(7U, &offset) == 1U);
    assert(offset == 168U);
    assert(zr_massivedoc_record_descriptor_offset(
               std::numeric_limits<std::uint64_t>::max(),
               &offset) == 0U);
    assert(zr_massivedoc_chunk_descriptor_offset(
               std::numeric_limits<std::uint64_t>::max(),
               &offset) == 0U);

    ZrMassiveDocSlicePlan plan{};
    assert(zr_massivedoc_plan_record_slice(100U, 25U, 40U, &plan) == 1U);
    assert(plan.offset == 25U && plan.length == 40U);
    assert(zr_massivedoc_plan_record_slice(100U, 90U, 40U, &plan) == 1U);
    assert(plan.offset == 90U && plan.length == 10U);
    assert(zr_massivedoc_plan_record_slice(100U, 101U, 1U, &plan) == 0U);

    assert(zr_massivedoc_chunk_range_within_segment(1024U, 1000U, 24U) == 1U);
    assert(zr_massivedoc_chunk_range_within_segment(1024U, 1000U, 25U) == 0U);
    assert(zr_massivedoc_record_chunk_span_within_table(8U, 4U, 12U) == 1U);
    assert(zr_massivedoc_record_chunk_span_within_table(8U, 5U, 12U) == 0U);
    assert(zr_massivedoc_record_chunk_span_within_table(
               std::numeric_limits<std::uint64_t>::max(),
               1U,
               std::numeric_limits<std::uint64_t>::max()) == 0U);

    return 0;
}
