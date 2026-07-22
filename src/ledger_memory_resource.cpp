#include "ledger_memory_resource.hpp"

#include <new>

namespace zevryon::core {

LedgerMemoryResource::LedgerMemoryResource(
    ResourceLedger& ledger,
    ResourceClass resource_class,
    std::pmr::memory_resource* upstream) noexcept
    : ledger_(&ledger),
      resource_class_(resource_class),
      upstream_(upstream != nullptr ? upstream : std::pmr::get_default_resource()) {}

ResourceLedger& LedgerMemoryResource::ledger() const noexcept {
    return *ledger_;
}

ResourceClass LedgerMemoryResource::resource_class() const noexcept {
    return resource_class_;
}

void* LedgerMemoryResource::do_allocate(std::size_t bytes, std::size_t alignment) {
    if (!ledger_->try_reserve(resource_class_, bytes)) {
        throw std::bad_alloc{};
    }
    try {
        return upstream_->allocate(bytes, alignment);
    } catch (...) {
        ledger_->release(resource_class_, bytes);
        throw;
    }
}

void LedgerMemoryResource::do_deallocate(
    void* pointer,
    std::size_t bytes,
    std::size_t alignment) {
    upstream_->deallocate(pointer, bytes, alignment);
    ledger_->release(resource_class_, bytes);
}

bool LedgerMemoryResource::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

} // namespace zevryon::core
