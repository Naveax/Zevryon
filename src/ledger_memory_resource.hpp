#pragma once

#include "resource_ledger.hpp"

#include <cstddef>
#include <memory_resource>

namespace zevryon::core {

class LedgerMemoryResource final : public std::pmr::memory_resource {
public:
    LedgerMemoryResource(
        ResourceLedger& ledger,
        ResourceClass resource_class,
        std::pmr::memory_resource* upstream = std::pmr::get_default_resource()) noexcept;

    ResourceLedger& ledger() const noexcept;
    ResourceClass resource_class() const noexcept;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    ResourceLedger* ledger_;
    ResourceClass resource_class_;
    std::pmr::memory_resource* upstream_;
};

} // namespace zevryon::core
