#include <cstdint>

#if defined(__linux__)
#include <sys/types.h>

#ifndef _FILE_OFFSET_BITS
#error "MassiveDoc Linux core requires _FILE_OFFSET_BITS=64"
#endif

#if _FILE_OFFSET_BITS != 64
#error "MassiveDoc Linux core requires 64-bit file offsets"
#endif

static_assert(
    sizeof(off_t) >= sizeof(std::int64_t),
    "MassiveDoc Linux core requires a 64-bit off_t ABI");
#endif
