#pragma once

#include "unicode_bidi_data.generated.hpp"

#include <cstdint>
#include <string_view>

namespace zevryon::text {

BidiClass bidi_class_of(std::uint32_t codepoint) noexcept;
std::string_view bidi_class_short_name(BidiClass value) noexcept;
std::string_view bidi_class_long_name(BidiClass value) noexcept;
bool bidi_class_from_name(std::string_view name, BidiClass* value) noexcept;
bool is_bidi_strong_left(BidiClass value) noexcept;
bool is_bidi_strong_right(BidiClass value) noexcept;
bool is_bidi_isolate_initiator(BidiClass value) noexcept;
bool is_bidi_explicit_formatting(BidiClass value) noexcept;

} // namespace zevryon::text
