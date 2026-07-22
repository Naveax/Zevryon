#pragma once

#include "unicode_script_data.generated.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zevryon::text {

inline constexpr std::uint16_t kNoScriptExtensionSet = 0xffffU;

class ScriptSetView {
public:
    constexpr ScriptSetView() noexcept = default;
    constexpr ScriptSetView(
        ScriptId primary,
        std::uint16_t extension_set_index) noexcept
        : primary_(primary), extension_set_index_(extension_set_index) {}

    std::size_t size() const noexcept;
    ScriptId operator[](std::size_t index) const noexcept;
    bool contains(ScriptId script) const noexcept;
    bool has_explicit_extensions() const noexcept;
    ScriptId primary() const noexcept;

private:
    ScriptId primary_{ScriptId::Zzzz};
    std::uint16_t extension_set_index_{kNoScriptExtensionSet};
};

ScriptId script_of(std::uint32_t codepoint) noexcept;
ScriptSetView script_extensions(std::uint32_t codepoint) noexcept;
bool is_neutral_script(ScriptId script) noexcept;
std::string_view script_short_name(ScriptId script) noexcept;
std::string_view script_long_name(ScriptId script) noexcept;
bool script_id_from_name(std::string_view name, ScriptId* script) noexcept;

} // namespace zevryon::text
