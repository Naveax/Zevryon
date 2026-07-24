#pragma once

#include "prepared_harfbuzz_face.hpp"

namespace zevryon::text {

// Internal backend bridge. The returned pointer is borrowed and remains valid
// only while the PreparedHarfBuzzFace is retained by the caller.
void* prepared_harfbuzz_native_face(
    const PreparedHarfBuzzFace& prepared) noexcept;

} // namespace zevryon::text
