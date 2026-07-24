#pragma once

#include "font_discovery_generation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zevryon::text {

enum class DirectWriteDiscoveryErrorKind : std::uint8_t {
    None = 0,
    FactoryCreationFailed,
    EnumerationFailed,
    MissingRequiredProperty,
    InvalidNativeValue,
    UnsupportedFontSource,
    ConflictingDuplicate,
    SnapshotBuildFailed
};

struct DirectWriteDiscoveryError {
    DirectWriteDiscoveryErrorKind kind{DirectWriteDiscoveryErrorKind::None};
    std::size_t family_index{0};
    std::size_t font_index{0};
    FontDiscoveryErrorKind generation_error{FontDiscoveryErrorKind::None};
    FontCatalogErrorKind catalog_error{FontCatalogErrorKind::None};
    long native_result{0};
    std::string message;
};

struct DirectWriteDiscoveryStats {
    std::uint64_t families_seen{0};
    std::uint64_t fonts_seen{0};
    std::uint64_t faces_emitted{0};
    std::uint64_t duplicate_faces_skipped{0};
    std::uint64_t font_files_seen{0};
    std::uint64_t coverage_codepoints{0};
    std::uint64_t coverage_ranges{0};
    std::uint64_t simulated_faces{0};
    std::uint64_t monospace_faces{0};
    FontDiscoveryStats generation;
};

const char* directwrite_discovery_error_kind_name(
    DirectWriteDiscoveryErrorKind kind) noexcept;

// Enumerates the current DirectWrite system font collection and immediately
// builds an immutable core generation. No COM object, native string, or font
// file reference escapes this call.
bool build_directwrite_generation(
    std::uint64_t generation_id,
    std::size_t discovery_hard_limit,
    std::size_t catalog_hard_limit,
    std::shared_ptr<const FontCatalogGeneration>* output,
    DirectWriteDiscoveryStats* stats,
    DirectWriteDiscoveryError* error) noexcept;

} // namespace zevryon::text
