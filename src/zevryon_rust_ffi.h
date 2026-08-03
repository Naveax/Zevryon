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

typedef struct ZR_ALIGNAS(ZR_LEDGER_STORAGE_ALIGN) ZrLedgerStorage {
    uint8_t bytes[ZR_LEDGER_STORAGE_BYTES];
} ZrLedgerStorage;

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

#if defined(__cplusplus)
}
#endif

#undef ZR_ALIGNAS
