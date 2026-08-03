#![forbid(unsafe_code)]
#![deny(warnings)]

use zevryon_abi::{ZrResourceSnapshot, ZR_RESOURCE_CLASS_COUNT};

pub const LEDGER_MAGIC: u64 = 0x5A52_4C45_4447_4552;

#[repr(C)]
pub struct ResourceLedger {
    magic: u64,
    total_current_bytes: usize,
    total_peak_bytes: usize,
    resources: [ZrResourceSnapshot; ZR_RESOURCE_CLASS_COUNT],
}

impl Default for ResourceLedger {
    fn default() -> Self {
        Self::new()
    }
}

impl ResourceLedger {
    pub fn new() -> Self {
        let mut resources = [ZrResourceSnapshot::default(); ZR_RESOURCE_CLASS_COUNT];
        for resource in &mut resources {
            resource.hard_limit_bytes = usize::MAX;
        }
        Self {
            magic: LEDGER_MAGIC,
            total_current_bytes: 0,
            total_peak_bytes: 0,
            resources,
        }
    }

    pub fn is_initialized(&self) -> bool {
        self.magic == LEDGER_MAGIC
    }

    pub fn set_hard_limit(&mut self, resource_class: u32, bytes: usize) -> bool {
        let Some(resource) = self.resource_mut(resource_class) else {
            return false;
        };
        resource.hard_limit_bytes = bytes;
        true
    }

    pub fn try_reserve(&mut self, resource_class: u32, bytes: usize) -> Option<bool> {
        let index = Self::index_of(resource_class)?;
        let resource = &mut self.resources[index];
        if bytes > resource.hard_limit_bytes
            || resource.current_bytes > resource.hard_limit_bytes - bytes
            || bytes > usize::MAX - self.total_current_bytes
        {
            resource.rejected_reservations = resource.rejected_reservations.wrapping_add(1);
            return Some(false);
        }

        resource.current_bytes += bytes;
        resource.peak_bytes = resource.peak_bytes.max(resource.current_bytes);
        resource.reservations = resource.reservations.wrapping_add(1);
        self.total_current_bytes += bytes;
        self.total_peak_bytes = self.total_peak_bytes.max(self.total_current_bytes);
        Some(true)
    }

    pub fn release(&mut self, resource_class: u32, bytes: usize) -> bool {
        let Some(index) = Self::index_of(resource_class) else {
            return false;
        };
        let resource = &mut self.resources[index];
        resource.releases = resource.releases.wrapping_add(1);
        if bytes > resource.current_bytes || bytes > self.total_current_bytes {
            resource.accounting_errors = resource.accounting_errors.wrapping_add(1);
            let correction = self.total_current_bytes.min(resource.current_bytes);
            self.total_current_bytes -= correction;
            resource.current_bytes = 0;
            return true;
        }

        resource.current_bytes -= bytes;
        self.total_current_bytes -= bytes;
        true
    }

    pub fn record_cache_hit(&mut self, resource_class: u32) -> bool {
        self.update_counter(resource_class, |snapshot| {
            snapshot.cache_hits = snapshot.cache_hits.wrapping_add(1);
        })
    }

    pub fn record_cache_miss(&mut self, resource_class: u32) -> bool {
        self.update_counter(resource_class, |snapshot| {
            snapshot.cache_misses = snapshot.cache_misses.wrapping_add(1);
        })
    }

    pub fn record_eviction(&mut self, resource_class: u32) -> bool {
        self.update_counter(resource_class, |snapshot| {
            snapshot.evictions = snapshot.evictions.wrapping_add(1);
        })
    }

    pub fn record_physical_read(&mut self, resource_class: u32, bytes: u64) -> bool {
        self.update_counter(resource_class, |snapshot| {
            snapshot.physical_read_bytes = snapshot.physical_read_bytes.saturating_add(bytes);
        })
    }

    pub fn record_physical_write(&mut self, resource_class: u32, bytes: u64) -> bool {
        self.update_counter(resource_class, |snapshot| {
            snapshot.physical_write_bytes = snapshot.physical_write_bytes.saturating_add(bytes);
        })
    }

    pub fn snapshot(&self, resource_class: u32) -> Option<ZrResourceSnapshot> {
        let index = Self::index_of(resource_class)?;
        Some(self.resources[index])
    }

    pub fn total_current_bytes(&self) -> usize {
        self.total_current_bytes
    }

    pub fn total_peak_bytes(&self) -> usize {
        self.total_peak_bytes
    }

    pub fn within_hard_limits(&self) -> bool {
        self.resources.iter().all(|resource| {
            resource.current_bytes <= resource.hard_limit_bytes
                && resource.peak_bytes <= resource.hard_limit_bytes
        })
    }

    pub fn accounting_clean(&self) -> bool {
        self.resources
            .iter()
            .all(|resource| resource.accounting_errors == 0)
    }

    fn index_of(resource_class: u32) -> Option<usize> {
        let index = usize::try_from(resource_class).ok()?;
        (index < ZR_RESOURCE_CLASS_COUNT).then_some(index)
    }

    fn resource_mut(&mut self, resource_class: u32) -> Option<&mut ZrResourceSnapshot> {
        let index = Self::index_of(resource_class)?;
        self.resources.get_mut(index)
    }

    fn update_counter(
        &mut self,
        resource_class: u32,
        update: impl FnOnce(&mut ZrResourceSnapshot),
    ) -> bool {
        let Some(resource) = self.resource_mut(resource_class) else {
            return false;
        };
        update(resource);
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SOURCE_WINDOW: u32 = 0;
    const CHECKPOINT_INDEX: u32 = 1;
    const NETWORK_BUFFER: u32 = 17;
    const LAYOUT_FRAGMENT: u32 = 11;

    #[test]
    fn matches_the_cpp_ledger_contract() {
        let mut ledger = ResourceLedger::new();
        assert!(ledger.is_initialized());
        assert!(ledger.set_hard_limit(SOURCE_WINDOW, 100));
        assert!(ledger.set_hard_limit(CHECKPOINT_INDEX, 40));

        assert_eq!(ledger.try_reserve(SOURCE_WINDOW, 60), Some(true));
        assert_eq!(ledger.try_reserve(SOURCE_WINDOW, 50), Some(false));
        assert_eq!(ledger.try_reserve(SOURCE_WINDOW, 40), Some(true));
        assert!(ledger.record_cache_hit(SOURCE_WINDOW));
        assert!(ledger.record_cache_miss(SOURCE_WINDOW));
        assert!(ledger.record_eviction(SOURCE_WINDOW));
        assert!(ledger.record_physical_read(SOURCE_WINDOW, 65_536));
        assert!(ledger.record_physical_write(SOURCE_WINDOW, 4_096));

        let snapshot = ledger.snapshot(SOURCE_WINDOW).expect("valid resource class");
        assert_eq!(snapshot.current_bytes, 100);
        assert_eq!(snapshot.peak_bytes, 100);
        assert_eq!(snapshot.reservations, 2);
        assert_eq!(snapshot.rejected_reservations, 1);
        assert_eq!(snapshot.cache_hits, 1);
        assert_eq!(snapshot.cache_misses, 1);
        assert_eq!(snapshot.evictions, 1);
        assert_eq!(snapshot.physical_read_bytes, 65_536);
        assert_eq!(snapshot.physical_write_bytes, 4_096);
        assert_eq!(ledger.total_current_bytes(), 100);
        assert_eq!(ledger.total_peak_bytes(), 100);
        assert!(ledger.within_hard_limits());
        assert!(ledger.accounting_clean());

        assert!(ledger.release(SOURCE_WINDOW, 40));
        assert!(ledger.release(SOURCE_WINDOW, 60));
        assert_eq!(ledger.total_current_bytes(), 0);

        assert!(ledger.record_physical_read(NETWORK_BUFFER, u64::MAX - 2));
        assert!(ledger.record_physical_read(NETWORK_BUFFER, 8));
        assert_eq!(
            ledger
                .snapshot(NETWORK_BUFFER)
                .expect("valid resource class")
                .physical_read_bytes,
            u64::MAX
        );

        assert!(ledger.set_hard_limit(LAYOUT_FRAGMENT, 32));
        assert_eq!(ledger.try_reserve(LAYOUT_FRAGMENT, 16), Some(true));
        assert!(ledger.release(LAYOUT_FRAGMENT, 17));
        assert!(!ledger.accounting_clean());
        assert_eq!(
            ledger
                .snapshot(LAYOUT_FRAGMENT)
                .expect("valid resource class")
                .current_bytes,
            0
        );
    }

    #[test]
    fn invalid_resource_classes_fail_closed() {
        let mut ledger = ResourceLedger::new();
        let invalid = u32::MAX;
        assert!(!ledger.set_hard_limit(invalid, 10));
        assert_eq!(ledger.try_reserve(invalid, 1), None);
        assert!(!ledger.release(invalid, 1));
        assert!(!ledger.record_cache_hit(invalid));
        assert!(ledger.snapshot(invalid).is_none());
    }
}
