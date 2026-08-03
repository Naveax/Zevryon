#include "zevryon_rust_ffi.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

_Static_assert(sizeof(ZrLedgerStorage) == ZR_LEDGER_STORAGE_BYTES, "ledger storage size");
_Static_assert(_Alignof(ZrLedgerStorage) == ZR_LEDGER_STORAGE_ALIGN, "ledger storage alignment");
_Static_assert(sizeof(((ZrLedgerStorage*)0)->bytes) == ZR_LEDGER_STORAGE_BYTES, "ledger payload size");

static int require(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", message);
        return 0;
    }
    return 1;
}

int main(void) {
    ZrLedgerStorage storage = {{0}};
    ZrResourceSnapshot snapshot = {0};

    if (!require(zr_abi_version() == ZR_ABI_VERSION, "ABI version") ||
        !require(zr_resource_class_count() == ZR_RESOURCE_CLASS_COUNT, "resource count") ||
        !require(zr_ledger_storage_size() == sizeof storage, "reported storage size") ||
        !require(
            zr_ledger_storage_alignment() == _Alignof(ZrLedgerStorage),
            "reported storage alignment") ||
        !require(zr_ledger_valid(&storage) == 0U, "zero storage is invalid") ||
        !require(zr_ledger_init(&storage) != 0U, "ledger initializes") ||
        !require(zr_ledger_valid(&storage) != 0U, "initialized ledger is valid") ||
        !require(zr_ledger_set_hard_limit(&storage, 0U, 100U) != 0U, "set hard limit") ||
        !require(zr_ledger_try_reserve(&storage, 0U, 60U) != 0U, "reserve below limit") ||
        !require(zr_ledger_try_reserve(&storage, 0U, 50U) == 0U, "reject overflow") ||
        !require(zr_ledger_snapshot(&storage, 0U, &snapshot) != 0U, "snapshot") ||
        !require(snapshot.current_bytes == 60U, "snapshot current bytes") ||
        !require(snapshot.peak_bytes == 60U, "snapshot peak bytes") ||
        !require(snapshot.rejected_reservations == 1U, "snapshot rejection count") ||
        !require(zr_ledger_release(&storage, 0U, 60U) != 0U, "release") ||
        !require(zr_ledger_total_current_bytes(&storage) == 0U, "aggregate release") ||
        !require(zr_ledger_within_hard_limits(&storage) != 0U, "hard limits clean") ||
        !require(zr_ledger_accounting_clean(&storage) != 0U, "accounting clean")) {
        return 1;
    }

    zr_ledger_clear(&storage);
    if (!require(zr_ledger_valid(&storage) == 0U, "cleared ledger is invalid")) {
        return 1;
    }

    puts("Rust C ABI ledger test passed");
    return 0;
}
