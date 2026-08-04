#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define ZR_MASSIVEDOC_ABI_VERSION 0x00010000u
#define ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES 32u
#define ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES 24u

typedef struct ZrMassiveDocRecordDescriptor {
    uint64_t logical_id;
    uint64_t first_chunk;
    uint64_t length;
    uint32_t chunk_count;
    uint32_t crc32;
} ZrMassiveDocRecordDescriptor;

typedef struct ZrMassiveDocChunkDescriptor {
    uint32_t segment_id;
    uint32_t reserved;
    uint64_t offset;
    uint64_t length;
} ZrMassiveDocChunkDescriptor;

typedef struct ZrMassiveDocSlicePlan {
    uint64_t offset;
    uint64_t length;
} ZrMassiveDocSlicePlan;

uint32_t zr_massivedoc_abi_version(void);
size_t zr_massivedoc_record_descriptor_size(void);
size_t zr_massivedoc_chunk_descriptor_size(void);
uint8_t zr_massivedoc_encode_record_descriptor(
    const ZrMassiveDocRecordDescriptor* descriptor,
    uint8_t* output,
    size_t output_len);
uint8_t zr_massivedoc_decode_record_descriptor(
    const uint8_t* input,
    size_t input_len,
    ZrMassiveDocRecordDescriptor* output);
uint8_t zr_massivedoc_encode_chunk_descriptor(
    const ZrMassiveDocChunkDescriptor* descriptor,
    uint8_t* output,
    size_t output_len);
uint8_t zr_massivedoc_decode_chunk_descriptor(
    const uint8_t* input,
    size_t input_len,
    ZrMassiveDocChunkDescriptor* output);
uint8_t zr_massivedoc_record_descriptor_offset(
    uint64_t record_index,
    uint64_t* output);
uint8_t zr_massivedoc_chunk_descriptor_offset(
    uint64_t chunk_index,
    uint64_t* output);
uint8_t zr_massivedoc_plan_record_slice(
    uint64_t record_length,
    uint64_t byte_offset,
    uint64_t max_bytes,
    ZrMassiveDocSlicePlan* output);
uint8_t zr_massivedoc_chunk_range_within_segment(
    uint64_t segment_bytes,
    uint64_t offset,
    uint64_t length);
uint8_t zr_massivedoc_record_chunk_span_within_table(
    uint64_t first_chunk,
    uint32_t chunk_count,
    uint64_t total_chunks);

#if defined(__cplusplus)
}

static_assert(sizeof(ZrMassiveDocRecordDescriptor) == ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES);
static_assert(sizeof(ZrMassiveDocChunkDescriptor) == ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES);
static_assert(sizeof(ZrMassiveDocSlicePlan) == 16u);
#else
_Static_assert(sizeof(ZrMassiveDocRecordDescriptor) == ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES, "record descriptor ABI");
_Static_assert(sizeof(ZrMassiveDocChunkDescriptor) == ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES, "chunk descriptor ABI");
_Static_assert(sizeof(ZrMassiveDocSlicePlan) == 16u, "slice plan ABI");
#endif
