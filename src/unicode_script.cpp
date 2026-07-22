#include "unicode_script.hpp"

#include <algorithm>

namespace zevryon::text {
namespace {

template <typename Range, std::size_t Size>
const Range* find_range(
    const std::array<Range, Size>& ranges,
    std::uint32_t codepoint) noexcept {
    std::size_t first = 0U;
    std::size_t count = ranges.size();
    while (count != 0U) {
        const std::size_t step = count / 2U;
        const std::size_t middle = first + step;
        if (ranges[middle].end < codepoint) {
            first = middle + 1U;
            count -= step + 1U;
        } else {
            count = step;
        }
    }
    if (first < ranges.size() &&
        ranges[first].start <= codepoint &&
        codepoint <= ranges[first].end) {
        return &ranges[first];
    }
    return nullptr;
}

bool valid_script_index(ScriptId script) noexcept {
    return static_cast<std::size_t>(script) <
           static_cast<std::size_t>(ScriptId::Count);
}

} // namespace

std::size_t ScriptSetView::size() const noexcept {
    if (extension_set_index_ == kNoScriptExtensionSet) {
        return 1U;
    }
    if (extension_set_index_ >= kScriptExtensionSets.size()) {
        return 0U;
    }
    return kScriptExtensionSets[extension_set_index_].count;
}

ScriptId ScriptSetView::operator[](std::size_t index) const noexcept {
    if (extension_set_index_ == kNoScriptExtensionSet) {
        return index == 0U ? primary_ : ScriptId::Zzzz;
    }
    if (extension_set_index_ >= kScriptExtensionSets.size()) {
        return ScriptId::Zzzz;
    }
    const ScriptSetRecord record = kScriptExtensionSets[extension_set_index_];
    if (index >= record.count ||
        record.offset > kScriptExtensionPool.size() ||
        index > kScriptExtensionPool.size() - record.offset) {
        return ScriptId::Zzzz;
    }
    const std::size_t pool_index =
        static_cast<std::size_t>(record.offset) + index;
    return pool_index < kScriptExtensionPool.size()
        ? kScriptExtensionPool[pool_index]
        : ScriptId::Zzzz;
}

bool ScriptSetView::contains(ScriptId script) const noexcept {
    for (std::size_t index = 0U; index < size(); ++index) {
        if ((*this)[index] == script) {
            return true;
        }
    }
    return false;
}

bool ScriptSetView::has_explicit_extensions() const noexcept {
    return extension_set_index_ != kNoScriptExtensionSet;
}

ScriptId ScriptSetView::primary() const noexcept {
    return primary_;
}

ScriptId script_of(std::uint32_t codepoint) noexcept {
    if (codepoint > 0x10ffffU) {
        return ScriptId::Zzzz;
    }
    const ScriptRange* range = find_range(kScriptRanges, codepoint);
    return range == nullptr ? ScriptId::Zzzz : range->script;
}

ScriptSetView script_extensions(std::uint32_t codepoint) noexcept {
    const ScriptId primary = script_of(codepoint);
    if (codepoint > 0x10ffffU) {
        return ScriptSetView(primary, kNoScriptExtensionSet);
    }
    const ScriptExtensionRange* range =
        find_range(kScriptExtensionRanges, codepoint);
    return range == nullptr
        ? ScriptSetView(primary, kNoScriptExtensionSet)
        : ScriptSetView(primary, range->set_index);
}

bool is_neutral_script(ScriptId script) noexcept {
    return script == ScriptId::Zyyy ||
           script == ScriptId::Zinh ||
           script == ScriptId::Zzzz;
}

std::string_view script_short_name(ScriptId script) noexcept {
    const std::size_t index = static_cast<std::size_t>(script);
    return valid_script_index(script) ? kScriptShortNames[index] : std::string_view{};
}

std::string_view script_long_name(ScriptId script) noexcept {
    const std::size_t index = static_cast<std::size_t>(script);
    return valid_script_index(script) ? kScriptLongNames[index] : std::string_view{};
}

bool script_id_from_name(std::string_view name, ScriptId* script) noexcept {
    if (script == nullptr) {
        return false;
    }
    for (std::size_t index = 0U; index < kScriptShortNames.size(); ++index) {
        if (kScriptShortNames[index] == name || kScriptLongNames[index] == name) {
            *script = static_cast<ScriptId>(index);
            return true;
        }
    }
    return false;
}

} // namespace zevryon::text
