#pragma once

#include "grapheme_segmenter.hpp"
#include "unicode_script.hpp"
#include "unicode_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class ScriptRunErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    IndexOverflow,
    OutputBudgetExceeded
};

const char* script_run_error_kind_name(ScriptRunErrorKind kind) noexcept;

struct ScriptRunError {
    ScriptRunErrorKind kind{ScriptRunErrorKind::None};
    std::size_t cluster_index{0};
    std::string message;
};

struct ScriptRunBoundary {
    std::uint64_t source_offset{0};
    std::uint32_t cluster_index{0};
    ScriptId script{ScriptId::Zzzz};
    std::uint16_t reserved{0};

    bool operator==(const ScriptRunBoundary&) const noexcept = default;
};

static_assert(
    sizeof(ScriptRunBoundary) <= 16U,
    "script-run boundary records must remain within the Z1 memory contract");

struct ScriptRunStats {
    std::uint64_t input_codepoints{0};
    std::uint64_t input_clusters{0};
    std::uint64_t output_runs{0};
    std::uint64_t neutral_clusters{0};
    std::uint64_t explicit_extension_lookups{0};
    std::uint64_t internal_cluster_conflicts{0};
    std::uint64_t run_splits{0};
    std::uint64_t maximum_run_clusters{0};
};

// Non-empty input produces one boundary for every run start plus one final
// sentinel. Run i spans boundaries[i]..boundaries[i + 1]. Script selection is
// shaping-oriented and does not perform bidi reordering.
bool resolve_script_runs(
    std::span<const DecodedCodePoint> codepoints,
    std::span<const GraphemeBoundary> grapheme_boundaries,
    std::pmr::vector<ScriptRunBoundary>* run_boundaries,
    ScriptRunStats* stats,
    ScriptRunError* error) noexcept;

} // namespace zevryon::text
