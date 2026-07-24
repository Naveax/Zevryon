#pragma once

#include "font_discovery_generation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zevryon::text {

enum class FontconfigDiscoveryErrorKind : std::uint8_t {
    None = 0,
    InitializationFailed,
    EnumerationFailed,
    MissingRequiredProperty,
    InvalidNativeValue,
    ConflictingDuplicate,
    SnapshotBuildFailed
};

struct FontconfigDiscoveryError {
    FontconfigDiscoveryErrorKind kind{FontconfigDiscoveryErrorKind::None};
    std::size_t pattern_index{0};
    FontDiscoveryErrorKind generation_error{FontDiscoveryErrorKind::None};
    FontCatalogErrorKind catalog_error{FontCatalogErrorKind::None};
    std::string message;
};

struct FontconfigDiscoveryStats {
    std::uint64_t fontconfig_version{0};
    std::uint64_t patterns_seen{0};
    std::uint64_t faces_emitted{0};
    std::uint64_t duplicate_patterns_skipped{0};
    std::uint64_t charset_codepoints{0};
    std::uint64_t coverage_ranges{0};
    std::uint64_t variable_faces{0};
    std::uint64_t color_faces{0};
    std::uint64_t monospace_faces{0};
    FontDiscoveryStats generation;
};

const char* fontconfig_discovery_error_kind_name(
    FontconfigDiscoveryErrorKind kind) noexcept;

// Enumerates the current Fontconfig configuration and immediately builds an
// immutable core generation. No FcPattern, FcCharSet, or Fontconfig-owned string
// escapes this call.
bool build_fontconfig_generation(
    std::uint64_t generation_id,
    std::size_t discovery_hard_limit,
    std::size_t catalog_hard_limit,
    std::shared_ptr<const FontCatalogGeneration>* output,
    FontconfigDiscoveryStats* stats,
    FontconfigDiscoveryError* error) noexcept;

} // namespace zevryon::text
