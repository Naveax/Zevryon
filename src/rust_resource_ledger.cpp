#include "rust_resource_ledger.hpp"

namespace zevryon::core {

RustResourceLedger::RustResourceLedger() noexcept
    : initialized_(zr_ledger_init(&storage_) != 0U) {}

RustResourceLedger::~RustResourceLedger() {
    zr_ledger_clear(&storage_);
    initialized_ = false;
}

bool RustResourceLedger::valid() const noexcept {
    return initialized_ && zr_ledger_valid(&storage_) != 0U;
}

bool RustResourceLedger::set_hard_limit(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    return valid() &&
        zr_ledger_set_hard_limit(&storage_, class_id(resource_class), bytes) != 0U;
}

bool RustResourceLedger::try_reserve(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    return valid() &&
        zr_ledger_try_reserve(&storage_, class_id(resource_class), bytes) != 0U;
}

bool RustResourceLedger::release(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    return valid() && zr_ledger_release(&storage_, class_id(resource_class), bytes) != 0U;
}

bool RustResourceLedger::record_cache_hit(ResourceClass resource_class) noexcept {
    return valid() &&
        zr_ledger_record_cache_hit(&storage_, class_id(resource_class)) != 0U;
}

bool RustResourceLedger::record_cache_miss(ResourceClass resource_class) noexcept {
    return valid() &&
        zr_ledger_record_cache_miss(&storage_, class_id(resource_class)) != 0U;
}

bool RustResourceLedger::record_eviction(ResourceClass resource_class) noexcept {
    return valid() &&
        zr_ledger_record_eviction(&storage_, class_id(resource_class)) != 0U;
}

bool RustResourceLedger::record_physical_read(
    ResourceClass resource_class,
    std::uint64_t bytes) noexcept {
    return valid() &&
        zr_ledger_record_physical_read(&storage_, class_id(resource_class), bytes) != 0U;
}

bool RustResourceLedger::record_physical_write(
    ResourceClass resource_class,
    std::uint64_t bytes) noexcept {
    return valid() &&
        zr_ledger_record_physical_write(&storage_, class_id(resource_class), bytes) != 0U;
}

bool RustResourceLedger::snapshot(
    ResourceClass resource_class,
    ResourceSnapshot& output) const noexcept {
    if (!valid()) {
        return false;
    }

    ZrResourceSnapshot snapshot{};
    if (zr_ledger_snapshot(&storage_, class_id(resource_class), &snapshot) == 0U) {
        return false;
    }

    output.hard_limit_bytes = snapshot.hard_limit_bytes;
    output.current_bytes = snapshot.current_bytes;
    output.peak_bytes = snapshot.peak_bytes;
    output.reservations = snapshot.reservations;
    output.releases = snapshot.releases;
    output.rejected_reservations = snapshot.rejected_reservations;
    output.accounting_errors = snapshot.accounting_errors;
    output.cache_hits = snapshot.cache_hits;
    output.cache_misses = snapshot.cache_misses;
    output.evictions = snapshot.evictions;
    output.physical_read_bytes = snapshot.physical_read_bytes;
    output.physical_write_bytes = snapshot.physical_write_bytes;
    return true;
}

std::size_t RustResourceLedger::total_current_bytes() const noexcept {
    return valid() ? zr_ledger_total_current_bytes(&storage_) : 0U;
}

std::size_t RustResourceLedger::total_peak_bytes() const noexcept {
    return valid() ? zr_ledger_total_peak_bytes(&storage_) : 0U;
}

bool RustResourceLedger::within_hard_limits() const noexcept {
    return valid() && zr_ledger_within_hard_limits(&storage_) != 0U;
}

bool RustResourceLedger::accounting_clean() const noexcept {
    return valid() && zr_ledger_accounting_clean(&storage_) != 0U;
}

std::uint32_t RustResourceLedger::abi_version() noexcept {
    return zr_abi_version();
}

std::uint32_t RustResourceLedger::ffi_resource_class_count() noexcept {
    return zr_resource_class_count();
}

std::uint32_t RustResourceLedger::class_id(ResourceClass resource_class) noexcept {
    return static_cast<std::uint32_t>(resource_class);
}

} // namespace zevryon::core
