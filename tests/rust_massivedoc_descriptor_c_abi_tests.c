#include "zevryon_massivedoc_rust_ffi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_condition(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "Rust MassiveDoc C ABI failure: %s\n", message);
        exit(1);
    }
}

int main(void) {
    ZrMassiveDocRecordDescriptor record = {
        UINT64_C(0x0102030405060708),
        UINT64_C(0x1112131415161718),
        UINT64_C(0x2122232425262728),
        UINT32_C(0x31323334),
        UINT32_C(0x41424344)};
    uint8_t bytes[ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES] = {0};
    ZrMassiveDocRecordDescriptor decoded = {0};
    uint64_t offset = 0;
    ZrMassiveDocSlicePlan plan = {0};

    require_condition(
        zr_massivedoc_abi_version() == ZR_MASSIVEDOC_ABI_VERSION,
        "ABI version mismatch");
    require_condition(
        zr_massivedoc_record_descriptor_size() == ZR_MASSIVEDOC_RECORD_DESCRIPTOR_BYTES,
        "record descriptor size mismatch");
    require_condition(
        zr_massivedoc_chunk_descriptor_size() == ZR_MASSIVEDOC_CHUNK_DESCRIPTOR_BYTES,
        "chunk descriptor size mismatch");
    require_condition(
        zr_massivedoc_encode_record_descriptor(&record, bytes, sizeof(bytes)) == UINT8_C(1),
        "record encode rejected");
    require_condition(bytes[0] == UINT8_C(0x08) && bytes[7] == UINT8_C(0x01), "logical id byte order");
    require_condition(bytes[24] == UINT8_C(0x34) && bytes[27] == UINT8_C(0x31), "chunk count byte order");
    require_condition(bytes[28] == UINT8_C(0x44) && bytes[31] == UINT8_C(0x41), "CRC byte order");
    require_condition(
        zr_massivedoc_decode_record_descriptor(bytes, sizeof(bytes), &decoded) == UINT8_C(1),
        "record decode rejected");
    require_condition(memcmp(&record, &decoded, sizeof(record)) == 0, "record round trip mismatch");
    require_condition(
        zr_massivedoc_record_descriptor_offset(UINT64_C(7), &offset) == UINT8_C(1) &&
            offset == UINT64_C(224),
        "record offset mismatch");
    require_condition(
        zr_massivedoc_plan_record_slice(UINT64_C(100), UINT64_C(90), UINT64_C(40), &plan) ==
                UINT8_C(1) &&
            plan.offset == UINT64_C(90) && plan.length == UINT64_C(10),
        "bounded slice mismatch");
    require_condition(
        zr_massivedoc_plan_record_slice(UINT64_C(100), UINT64_C(101), UINT64_C(1), &plan) ==
            UINT8_C(0),
        "invalid slice accepted");
    require_condition(
        zr_massivedoc_encode_record_descriptor(NULL, bytes, sizeof(bytes)) == UINT8_C(0),
        "null descriptor accepted");
    require_condition(
        zr_massivedoc_decode_record_descriptor(bytes, sizeof(bytes) - 1U, &decoded) == UINT8_C(0),
        "short descriptor accepted");

    puts("Rust MassiveDoc C ABI passed");
    return 0;
}
