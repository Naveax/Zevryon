#![forbid(unsafe_code)]
#![deny(warnings)]

pub const ZR_ABI_VERSION: u32 = 0x0001_0000;
pub const ZR_RESOURCE_CLASS_COUNT: usize = 36;
pub const ZR_LEDGER_STORAGE_BYTES: usize = 4096;
pub const ZR_LEDGER_STORAGE_ALIGN: usize = 8;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ZrResourceSnapshot {
    pub hard_limit_bytes: usize,
    pub current_bytes: usize,
    pub peak_bytes: usize,
    pub reservations: u64,
    pub releases: u64,
    pub rejected_reservations: u64,
    pub accounting_errors: u64,
    pub cache_hits: u64,
    pub cache_misses: u64,
    pub evictions: u64,
    pub physical_read_bytes: u64,
    pub physical_write_bytes: u64,
}

#[repr(C, align(8))]
#[derive(Clone, Copy)]
pub struct ZrLedgerStorage {
    pub bytes: [u8; ZR_LEDGER_STORAGE_BYTES],
}

impl Default for ZrLedgerStorage {
    fn default() -> Self {
        Self {
            bytes: [0; ZR_LEDGER_STORAGE_BYTES],
        }
    }
}

#[cfg(target_pointer_width = "64")]
const _: [(); 96] = [(); core::mem::size_of::<ZrResourceSnapshot>()];
const _: [(); ZR_LEDGER_STORAGE_BYTES] = [(); core::mem::size_of::<ZrLedgerStorage>()];
const _: [(); ZR_LEDGER_STORAGE_ALIGN] = [(); core::mem::align_of::<ZrLedgerStorage>()];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn abi_records_are_stable() {
        assert_eq!(ZR_RESOURCE_CLASS_COUNT, 36);
        assert_eq!(core::mem::size_of::<ZrLedgerStorage>(), 4096);
        assert_eq!(core::mem::align_of::<ZrLedgerStorage>(), 8);
        #[cfg(target_pointer_width = "64")]
        assert_eq!(core::mem::size_of::<ZrResourceSnapshot>(), 96);
    }
}
