#include "massivedoc_address_space.hpp"
#include "massivedoc_cold_window.hpp"
#include "massivedoc_store.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        fail(message);
    }
}

} // namespace

int main() {
    constexpr AddressSpaceWindowLimits bits32 =
        address_space_window_limits_for_pointer_bits(32U);
    static_assert(bits32.valid());
    static_assert(bits32.maximum_io_window_bytes == 4U * 1024U * 1024U);
    static_assert(bits32.maximum_mapped_window_bytes == 8U * 1024U * 1024U);
    static_assert(bits32.maximum_materialized_slice_bytes == 8U * 1024U * 1024U);

    constexpr AddressSpaceWindowLimits bits64 =
        address_space_window_limits_for_pointer_bits(64U);
    static_assert(bits64.valid());
    static_assert(bits64.maximum_io_window_bytes == 16U * 1024U * 1024U);
    static_assert(bits64.maximum_mapped_window_bytes == 16U * 1024U * 1024U);
    static_assert(bits64.maximum_materialized_slice_bytes == 64U * 1024U * 1024U);

    constexpr AddressSpaceWindowLimits current =
        current_address_space_window_limits();
    require(current.valid(), "current address-space policy is invalid");
    require(
        kMaximumIoWindowBytes == current.maximum_io_window_bytes,
        "StoreReader I/O cap is not bound to the current address-space policy");
    require(
        kMaximumColdMappedWindowBytes == current.maximum_mapped_window_bytes,
        "cold mmap cap is not bound to the current address-space policy");
    require(
        kMaximumMaterializedRecordSliceBytes == current.maximum_materialized_slice_bytes,
        "record-slice policy cap is not exported by StoreReader");
    require(
        kIoWindowBytes <= kMaximumIoWindowBytes,
        "default I/O window exceeds the address-space cap");

    std::cout << "Zevryon address-space window policy tests passed\n";
    return 0;
}
