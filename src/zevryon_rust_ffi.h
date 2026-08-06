#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
#define ZR_ALIGNAS(value) alignas(value)
extern "C" {
#else
#define ZR_ALIGNAS(value) _Alignas(value)
#endif

#define ZR_ABI_VERSION 0x00010000u
#define ZR_RESOURCE_CLASS_COUNT 36u
#define ZR_LEDGER_STORAGE_BYTES 4096u
#define ZR_LEDGER_STORAGE_ALIGN 8u

#define ZR_UTF8_ABI_VERSION 0x00010001u
#define ZR_UTF8_DECODER_STORAGE_BYTES 128u
#define ZR_UTF8_DECODER_STORAGE_ALIGN 8u
#define ZR_UTF8_POLICY_STRICT 0u
#define ZR_UTF8_POLICY_REPLACE 1u
#define ZR_UTF8_ERROR_NONE 0u
#define ZR_UTF8_ERROR_DISCONTINUOUS_INPUT 1u
#define ZR_UTF8_ERROR_INVALID_LEAD_BYTE 2u
#define ZR_UTF8_ERROR_UNEXPECTED_CONTINUATION 3u
#define ZR_UTF8_ERROR_INVALID_CONTINUATION 4u
#define ZR_UTF8_ERROR_OVERLONG_ENCODING 5u
#define ZR_UTF8_ERROR_SURROGATE_CODE_POINT 6u
#define ZR_UTF8_ERROR_CODE_POINT_OUT_OF_RANGE 7u
#define ZR_UTF8_ERROR_TRUNCATED_SEQUENCE 8u
#define ZR_UTF8_ERROR_OUTPUT_BUDGET_EXCEEDED 9u

#define ZR_UTF8_ERROR_DETAIL_NONE 0u
#define ZR_UTF8_ERROR_DETAIL_DECODER_FAILED 1u
#define ZR_UTF8_ERROR_DETAIL_DECODER_FINISHED 2u
#define ZR_UTF8_ERROR_DETAIL_DISCONTINUOUS_OFFSET 3u
#define ZR_UTF8_ERROR_DETAIL_SOURCE_RANGE_OVERFLOW 4u
#define ZR_UTF8_ERROR_DETAIL_OUTPUT_CAPACITY 5u

typedef struct ZrResourceSnapshot {
    size_t hard_limit_bytes;
    size_t current_bytes;
    size_t peak_bytes;
    uint64_t reservations;
    uint64_t releases;
    uint64_t rejected_reservations;
    uint64_t accounting_errors;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t evictions;
    uint64_t physical_read_bytes;
    uint64_t physical_write_bytes;
} ZrResourceSnapshot;

typedef struct ZrLedgerStorage {
    ZR_ALIGNAS(ZR_LEDGER_STORAGE_ALIGN) uint8_t bytes[ZR_LEDGER_STORAGE_BYTES];
} ZrLedgerStorage;

typedef struct ZrDecodedCodePoint {
    uint64_t source_start;
    uint32_t value;
    uint8_t source_length;
    uint8_t replacement;
    uint8_t reserved[2];
} ZrDecodedCodePoint;

typedef struct ZrUtf8DecodeStats {
    uint64_t source_bytes;
    uint64_t emitted_codepoints;
    uint64_t invalid_sequences;
    uint64_t replacements;
    uint64_t chunks;
    uint8_t maximum_pending_continuations;
    uint8_t reserved[7];
} ZrUtf8DecodeStats;

typedef struct ZrUtf8DecodeError {
    uint32_t kind;
    uint32_t detail;
    uint64_t source_offset;
} ZrUtf8DecodeError;

typedef struct ZrUtf8DecoderStorage {
    ZR_ALIGNAS(ZR_UTF8_DECODER_STORAGE_ALIGN)
    uint8_t bytes[ZR_UTF8_DECODER_STORAGE_BYTES];
} ZrUtf8DecoderStorage;

uint32_t zr_abi_version(void);
uint32_t zr_resource_class_count(void);
size_t zr_ledger_storage_size(void);
size_t zr_ledger_storage_alignment(void);
uint8_t zr_ledger_init(ZrLedgerStorage* storage);
void zr_ledger_clear(ZrLedgerStorage* storage);
uint8_t zr_ledger_valid(const ZrLedgerStorage* storage);
uint8_t zr_ledger_set_hard_limit(
    ZrLedgerStorage* storage,
    uint32_t resource_class,
    size_t bytes);
uint8_t zr_ledger_try_reserve(
    ZrLedgerStorage* storage,
    uint32_t resource_class,
    size_t bytes);
uint8_t zr_ledger_release(
    ZrLedgerStorage* storage,
    uint32_t resource_class,
    size_t bytes);
uint8_t zr_ledger_record_cache_hit(
    ZrLedgerStorage* storage,
    uint32_t resource_class);
uint8_t zr_ledger_record_cache_miss(
    ZrLedgerStorage* storage,
    uint32_t resource_class);
uint8_t zr_ledger_record_eviction(
    ZrLedgerStorage* storage,
    uint32_t resource_class);
uint8_t zr_ledger_record_physical_read(
    ZrLedgerStorage* storage,
    uint32_t resource_class,
    uint64_t bytes);
uint8_t zr_ledger_record_physical_write(
    ZrLedgerStorage* storage,
    uint32_t resource_class,
    uint64_t bytes);
uint8_t zr_ledger_snapshot(
    const ZrLedgerStorage* storage,
    uint32_t resource_class,
    ZrResourceSnapshot* output);
size_t zr_ledger_total_current_bytes(const ZrLedgerStorage* storage);
size_t zr_ledger_total_peak_bytes(const ZrLedgerStorage* storage);
uint8_t zr_ledger_within_hard_limits(const ZrLedgerStorage* storage);
uint8_t zr_ledger_accounting_clean(const ZrLedgerStorage* storage);

uint32_t zr_utf8_abi_version(void);
size_t zr_utf8_decoder_storage_size(void);
size_t zr_utf8_decoder_storage_alignment(void);
uint8_t zr_utf8_decoder_init(
    ZrUtf8DecoderStorage* storage,
    uint32_t policy);
void zr_utf8_decoder_clear(ZrUtf8DecoderStorage* storage);
uint8_t zr_utf8_decoder_valid(const ZrUtf8DecoderStorage* storage);
uint8_t zr_utf8_decoder_feed(
    ZrUtf8DecoderStorage* storage,
    const uint8_t* bytes,
    size_t byte_count,
    uint64_t absolute_source_offset,
    ZrDecodedCodePoint* output,
    size_t output_capacity,
    size_t* written,
    ZrUtf8DecodeError* error);
uint8_t zr_utf8_decoder_finish(
    ZrUtf8DecoderStorage* storage,
    ZrDecodedCodePoint* output,
    size_t output_capacity,
    size_t* written,
    ZrUtf8DecodeError* error);
uint8_t zr_utf8_decoder_reset(ZrUtf8DecoderStorage* storage);
uint32_t zr_utf8_decoder_policy(const ZrUtf8DecoderStorage* storage);
uint8_t zr_utf8_decoder_stats(
    const ZrUtf8DecoderStorage* storage,
    ZrUtf8DecodeStats* output);
uint64_t zr_utf8_decoder_next_source_offset(
    const ZrUtf8DecoderStorage* storage);
uint8_t zr_utf8_decoder_failed(const ZrUtf8DecoderStorage* storage);

#if defined(__cplusplus)
}
#endif

#undef ZR_ALIGNAS
