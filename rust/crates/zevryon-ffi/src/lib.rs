#![deny(unsafe_op_in_unsafe_fn)]
#![deny(warnings)]
// C ABI pointer validity is checked for null and alignment before each audited
// unsafe block. Writability and lifetime remain the foreign caller's contract.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use core::mem::{align_of, size_of};
use core::ptr;
use zevryon_abi::{
    ZrLedgerStorage, ZrResourceSnapshot, ZR_ABI_VERSION, ZR_LEDGER_STORAGE_ALIGN,
    ZR_LEDGER_STORAGE_BYTES, ZR_RESOURCE_CLASS_COUNT,
};
use zevryon_ledger::ResourceLedger;

const _: [(); 1] = [(); (size_of::<ResourceLedger>() <= ZR_LEDGER_STORAGE_BYTES) as usize];
const _: [(); 1] = [(); (align_of::<ResourceLedger>() <= ZR_LEDGER_STORAGE_ALIGN) as usize];

fn storage_aligned(storage: *const ZrLedgerStorage) -> bool {
    !storage.is_null()
        && (storage as usize).is_multiple_of(align_of::<ResourceLedger>())
}

fn snapshot_aligned(snapshot: *const ZrResourceSnapshot) -> bool {
    !snapshot.is_null()
        && (snapshot as usize).is_multiple_of(align_of::<ZrResourceSnapshot>())
}

fn with_ledger<R>(
    storage: *const ZrLedgerStorage,
    operation: impl FnOnce(&ResourceLedger) -> R,
) -> Option<R> {
    if !storage_aligned(storage) {
        return None;
    }
    let ledger = unsafe {
        // SAFETY: The pointer is non-null and aligned. Callers must initialize the
        // storage through zr_ledger_init and keep it alive for this call.
        &*storage.cast::<ResourceLedger>()
    };
    ledger.is_initialized().then(|| operation(ledger))
}

fn with_ledger_mut<R>(
    storage: *mut ZrLedgerStorage,
    operation: impl FnOnce(&mut ResourceLedger) -> R,
) -> Option<R> {
    if !storage_aligned(storage) {
        return None;
    }
    let ledger = unsafe {
        // SAFETY: The pointer is non-null and aligned. The C ABI requires exclusive
        // access to the storage for every mutating call.
        &mut *storage.cast::<ResourceLedger>()
    };
    ledger.is_initialized().then(|| operation(ledger))
}

#[no_mangle]
pub extern "C" fn zr_abi_version() -> u32 {
    ZR_ABI_VERSION
}

#[no_mangle]
pub extern "C" fn zr_resource_class_count() -> u32 {
    u32::try_from(ZR_RESOURCE_CLASS_COUNT).expect("resource class count fits in u32")
}

#[no_mangle]
pub extern "C" fn zr_ledger_storage_size() -> usize {
    ZR_LEDGER_STORAGE_BYTES
}

#[no_mangle]
pub extern "C" fn zr_ledger_storage_alignment() -> usize {
    ZR_LEDGER_STORAGE_ALIGN
}

#[no_mangle]
pub extern "C" fn zr_ledger_init(storage: *mut ZrLedgerStorage) -> u8 {
    if !storage_aligned(storage) {
        return 0;
    }
    unsafe {
        // SAFETY: The storage is non-null, aligned, and large enough by the compile-time
        // assertions above. ResourceLedger has no externally owned allocations.
        storage
            .cast::<ResourceLedger>()
            .write(ResourceLedger::new());
    }
    1
}

#[no_mangle]
pub extern "C" fn zr_ledger_clear(storage: *mut ZrLedgerStorage) {
    if !storage_aligned(storage) {
        return;
    }
    unsafe {
        // SAFETY: The storage points to exactly ZR_LEDGER_STORAGE_BYTES writable bytes.
        ptr::write_bytes(storage.cast::<u8>(), 0, ZR_LEDGER_STORAGE_BYTES);
    }
}

#[no_mangle]
pub extern "C" fn zr_ledger_valid(storage: *const ZrLedgerStorage) -> u8 {
    u8::from(with_ledger(storage, |_| ()).is_some())
}

#[no_mangle]
pub extern "C" fn zr_ledger_set_hard_limit(
    storage: *mut ZrLedgerStorage,
    resource_class: u32,
    bytes: usize,
) -> u8 {
    u8::from(
        with_ledger_mut(storage, |ledger| {
            ledger.set_hard_limit(resource_class, bytes)
        })
        .unwrap_or(false),
    )
}

#[no_mangle]
pub extern "C" fn zr_ledger_try_reserve(
    storage: *mut ZrLedgerStorage,
    resource_class: u32,
    bytes: usize,
) -> u8 {
    u8::from(
        with_ledger_mut(storage, |ledger| ledger.try_reserve(resource_class, bytes))
            .flatten()
            .unwrap_or(false),
    )
}

#[no_mangle]
pub extern "C" fn zr_ledger_release(
    storage: *mut ZrLedgerStorage,
    resource_class: u32,
    bytes: usize,
) -> u8 {
    u8::from(
        with_ledger_mut(storage, |ledger| ledger.release(resource_class, bytes)).unwrap_or(false),
    )
}

#[no_mangle]
pub extern "C" fn zr_ledger_record_cache_hit(
    storage: *mut ZrLedgerStorage,
    resource_class: u32,
) -> u8 {
    u8::from(
        with_ledger_mut(storage, |ledger| ledger.record_cache_hit(resource_class)).unwrap_or(false),
    )
}

#[no_mangle]
pub extern "C" fn zr_ledger_record_cache_miss(
    storage: *mut ZrLedgerStorage,
    resource_class: u32,
) -> u8 {
    u8::from(
        with_ledger_mut(storage, |ledger| ledger.record_cache_miss(resource_class))
            .unwrap_or(false),
    )
}

#[no_mangle]
pub extern "C" fn zr_ledger_record_eviction(
    storage: *mut ZrLedgerStorage,
    resource_class: u32,
) -> u8 {
    u8::from(
        with_ledger_mut(storage, |ledger| ledger.record_eviction(resource_class)).unwrap_or(false),
    )
}

#[no_mangle]
pub extern "C" fn zr_ledger_record_physical_read(
    storage: *mut ZrLedgerStorage,
    resource_class: u32,
    bytes: u64,
) -> u8 {
    u8::from(
        with_ledger_mut(storage, |ledger| {
            ledger.record_physical_read(resource_class, bytes)
        })
        .unwrap_or(false),
    )
}

#[no_mangle]
pub extern "C" fn zr_ledger_record_physical_write(
    storage: *mut ZrLedgerStorage,
    resource_class: u32,
    bytes: u64,
) -> u8 {
    u8::from(
        with_ledger_mut(storage, |ledger| {
            ledger.record_physical_write(resource_class, bytes)
        })
        .unwrap_or(false),
    )
}

#[no_mangle]
pub extern "C" fn zr_ledger_snapshot(
    storage: *const ZrLedgerStorage,
    resource_class: u32,
    output: *mut ZrResourceSnapshot,
) -> u8 {
    if !snapshot_aligned(output) {
        return 0;
    }
    let Some(snapshot) = with_ledger(storage, |ledger| ledger.snapshot(resource_class)).flatten()
    else {
        return 0;
    };
    unsafe {
        // SAFETY: The output pointer is non-null, aligned, and writable by the ABI contract.
        output.write(snapshot);
    }
    1
}

#[no_mangle]
pub extern "C" fn zr_ledger_total_current_bytes(storage: *const ZrLedgerStorage) -> usize {
    with_ledger(storage, ResourceLedger::total_current_bytes).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn zr_ledger_total_peak_bytes(storage: *const ZrLedgerStorage) -> usize {
    with_ledger(storage, ResourceLedger::total_peak_bytes).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn zr_ledger_within_hard_limits(storage: *const ZrLedgerStorage) -> u8 {
    u8::from(with_ledger(storage, ResourceLedger::within_hard_limits).unwrap_or(false))
}

#[no_mangle]
pub extern "C" fn zr_ledger_accounting_clean(storage: *const ZrLedgerStorage) -> u8 {
    u8::from(with_ledger(storage, ResourceLedger::accounting_clean).unwrap_or(false))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ffi_storage_lifecycle_is_fail_closed() {
        let mut storage = ZrLedgerStorage::default();
        assert_eq!(zr_ledger_valid(&storage), 0);
        assert_eq!(zr_ledger_init(&mut storage), 1);
        assert_eq!(zr_ledger_valid(&storage), 1);
        assert_eq!(zr_ledger_set_hard_limit(&mut storage, 0, 100), 1);
        assert_eq!(zr_ledger_try_reserve(&mut storage, 0, 60), 1);
        assert_eq!(zr_ledger_try_reserve(&mut storage, 0, 50), 0);

        let mut snapshot = ZrResourceSnapshot::default();
        assert_eq!(zr_ledger_snapshot(&storage, 0, &mut snapshot), 1);
        assert_eq!(snapshot.current_bytes, 60);
        assert_eq!(snapshot.rejected_reservations, 1);

        zr_ledger_clear(&mut storage);
        assert_eq!(zr_ledger_valid(&storage), 0);
        assert_eq!(zr_ledger_try_reserve(&mut storage, 0, 1), 0);
    }
}
