#include "massivedoc_address_space.hpp"
#include "massivedoc_cold_window.hpp"
#include "massivedoc_store.hpp"

#include <cstdint>
#include <sys/types.h>

#if !defined(__linux__) || !defined(__i386__)
#error "This probe must be compiled for a Linux i386 ABI"
#endif

#ifndef _FILE_OFFSET_BITS
#error "Linux i386 MassiveDoc probe requires _FILE_OFFSET_BITS=64"
#endif

#if _FILE_OFFSET_BITS != 64
#error "Linux i386 MassiveDoc probe requires 64-bit file offsets"
#endif

static_assert(sizeof(void*) == 4U, "Linux i386 probe must use 32-bit pointers");
static_assert(sizeof(off_t) >= sizeof(std::int64_t), "Linux i386 probe requires 64-bit off_t");

constexpr auto kI386Limits =
    zevryon::massivedoc::current_address_space_window_limits();
static_assert(kI386Limits.pointer_bits == 32U);
static_assert(kI386Limits.maximum_io_window_bytes == 4U * 1024U * 1024U);
static_assert(kI386Limits.maximum_mapped_window_bytes == 8U * 1024U * 1024U);
static_assert(kI386Limits.maximum_materialized_slice_bytes == 8U * 1024U * 1024U);
static_assert(
    zevryon::massivedoc::kMaximumIoWindowBytes ==
    kI386Limits.maximum_io_window_bytes);
static_assert(
    zevryon::massivedoc::kMaximumColdMappedWindowBytes ==
    kI386Limits.maximum_mapped_window_bytes);
static_assert(
    zevryon::massivedoc::kMaximumMaterializedRecordSliceBytes ==
    kI386Limits.maximum_materialized_slice_bytes);

int main() {
    return 0;
}
