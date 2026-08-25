#pragma once

#include <cstddef>
#include <cstdint>

namespace zevryon::massivedoc {

constexpr std::size_t kAddressSpaceMiB = 1024U * 1024U;

struct AddressSpaceWindowLimits {
    std::uint32_t pointer_bits{0U};
    std::size_t maximum_io_window_bytes{0U};
    std::size_t maximum_mapped_window_bytes{0U};
    std::size_t maximum_materialized_slice_bytes{0U};

    constexpr bool valid() const noexcept {
        return pointer_bits >= 32U &&
               maximum_io_window_bytes > 0U &&
               maximum_mapped_window_bytes >= maximum_io_window_bytes &&
               maximum_materialized_slice_bytes >= maximum_io_window_bytes;
    }
};

constexpr AddressSpaceWindowLimits address_space_window_limits_for_pointer_bits(
    std::uint32_t pointer_bits) noexcept {
    if (pointer_bits <= 32U) {
        return AddressSpaceWindowLimits{
            pointer_bits,
            4U * kAddressSpaceMiB,
            8U * kAddressSpaceMiB,
            8U * kAddressSpaceMiB};
    }
    return AddressSpaceWindowLimits{
        pointer_bits,
        16U * kAddressSpaceMiB,
        16U * kAddressSpaceMiB,
        64U * kAddressSpaceMiB};
}

constexpr std::uint32_t current_pointer_bits() noexcept {
    return static_cast<std::uint32_t>(sizeof(void*) * 8U);
}

constexpr AddressSpaceWindowLimits current_address_space_window_limits() noexcept {
    return address_space_window_limits_for_pointer_bits(current_pointer_bits());
}

constexpr bool materialized_slice_request_fits_limits(
    std::size_t bytes,
    AddressSpaceWindowLimits limits) noexcept {
    return limits.valid() && bytes <= limits.maximum_materialized_slice_bytes;
}

constexpr bool materialized_slice_request_fits_address_space(
    std::size_t bytes) noexcept {
    return materialized_slice_request_fits_limits(
        bytes,
        current_address_space_window_limits());
}

static_assert(current_pointer_bits() >= 32U,
              "Zevryon requires at least a 32-bit address space");
static_assert(current_address_space_window_limits().valid());

} // namespace zevryon::massivedoc
