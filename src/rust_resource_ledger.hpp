#pragma once

#include "resource_ledger.hpp"
#include "zevryon_rust_ffi.h"

#include <cstddef>
#include <cstdint>

namespace zevryon::core {

class RustResourceLedger final {
public:
    RustResourceLedger() noexcept;
    ~RustResourceLedger();

    RustResourceLedger(const RustResourceLedger&) = delete;
    RustResourceLedger& operator=(const RustResourceLedger&) = delete;
    RustResourceLedger(RustResourceLedger&&) = delete;
    RustResourceLedger& operator=(RustResourceLedger&&) = delete;

    bool valid() const noexcept;
    bool set_hard_limit(ResourceClass resource_class, std::size_t bytes) noexcept;
    bool try_reserve(ResourceClass resource_class, std::size_t bytes) noexcept;
    bool release(ResourceClass resource_class, std::size_t bytes) noexcept;

    bool record_cache_hit(ResourceClass resource_class) noexcept;
    bool record_cache_miss(ResourceClass resource_class) noexcept;
    bool record_eviction(ResourceClass resource_class) noexcept;
    bool record_physical_read(ResourceClass resource_class, std::uint64_t bytes) noexcept;
    bool record_physical_write(ResourceClass resource_class, std::uint64_t bytes) noexcept;

    bool snapshot(ResourceClass resource_class, ResourceSnapshot& output) const noexcept;
    std::size_t total_current_bytes() const noexcept;
    std::size_t total_peak_bytes() const noexcept;
    bool within_hard_limits() const noexcept;
    bool accounting_clean() const noexcept;

    static std::uint32_t abi_version() noexcept;
    static std::uint32_t ffi_resource_class_count() noexcept;

private:
    static std::uint32_t class_id(ResourceClass resource_class) noexcept;

    ZrLedgerStorage storage_{};
    bool initialized_{false};
};

static_assert(sizeof(ZrLedgerStorage) == ZR_LEDGER_STORAGE_BYTES);
static_assert(alignof(ZrLedgerStorage) == ZR_LEDGER_STORAGE_ALIGN);
static_assert(resource_class_count == ZR_RESOURCE_CLASS_COUNT);

} // namespace zevryon::core
