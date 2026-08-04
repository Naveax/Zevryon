#include "zevryon_massivedoc_rust_ffi.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    ZrMassiveDocRecordDescriptor record = {9u, 11u, 4096u, 3u, 0x12345678u};
    uint8_t record_bytes[ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES] = {0};
    ZrMassiveDocRecordDescriptor decoded_record = {0};
    ZrMassiveDocChunkDescriptor chunk = {7u, UINT32_MAX, 65536u, 4096u};
    uint8_t chunk_bytes[ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES] = {0};
    ZrMassiveDocChunkDescriptor decoded_chunk = {0};
    ZrMassiveDocSlicePlan plan = {0};
    uint64_t offset = 0u;

    assert(zr_massivedoc_abi_version() == ZR_MASSIVEDOC_ABI_VERSION);
    assert(zr_massivedoc_record_descriptor_size() == sizeof(record));
    assert(zr_massivedoc_chunk_descriptor_size() == sizeof(chunk));

    assert(zr_massivedoc_encode_record_descriptor(
               &record,
               record_bytes,
               sizeof(record_bytes)) == 1u);
    assert(zr_massivedoc_decode_record_descriptor(
               record_bytes,
               sizeof(record_bytes),
               &decoded_record) == 1u);
    assert(memcmp(&record, &decoded_record, sizeof(record)) == 0);

    assert(zr_massivedoc_encode_chunk_descriptor(
               &chunk,
               chunk_bytes,
               sizeof(chunk_bytes)) == 1u);
    assert(chunk_bytes[4] == 0u && chunk_bytes[5] == 0u &&
           chunk_bytes[6] == 0u && chunk_bytes[7] == 0u);
    assert(zr_massivedoc_decode_chunk_descriptor(
               chunk_bytes,
               sizeof(chunk_bytes),
               &decoded_chunk) == 1u);
    assert(decoded_chunk.segment_id == chunk.segment_id);
    assert(decoded_chunk.reserved == 0u);
    assert(decoded_chunk.offset == chunk.offset);
    assert(decoded_chunk.length == chunk.length);

    assert(zr_massivedoc_record_descriptor_offset(3u, &offset) == 1u);
    assert(offset == 96u);
    assert(zr_massivedoc_chunk_descriptor_offset(3u, &offset) == 1u);
    assert(offset == 72u);
    assert(zr_massivedoc_plan_record_slice(100u, 90u, 40u, &plan) == 1u);
    assert(plan.offset == 90u && plan.length == 10u);
    assert(zr_massivedoc_plan_record_slice(100u, 101u, 1u, &plan) == 0u);
    assert(zr_massivedoc_chunk_range_within_segment(1024u, 1000u, 24u) == 1u);
    assert(zr_massivedoc_chunk_range_within_segment(1024u, 1000u, 25u) == 0u);

    return 0;
}
