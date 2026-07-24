#pragma once

#include "font_discovery_generation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zevryon::text {

enum class CoreTextDiscoveryErrorKind : std::uint8_t {
    None = 0,
    CollectionCreationFailed,
    EnumerationFailed,
    MissingRequiredProperty,
    InvalidNativeValue,
    ConflictingDuplicate,
    SnapshotBuildFailed
};

struct CoreTextDiscoveryError {
    CoreTextDiscoveryErrorKind kind{CoreTextDiscoveryErrorKind::None};
    std::size_t descriptor_index{0};
    FontDiscoveryErrorKind generation_error{FontDiscoveryErrorKind::None};
    FontCatalogErrorKind catalog_error{FontCatalogErrorKind::None};
    std::string message;
};

struct CoreTextDiscoveryStats {
    std::uint64_t descriptors_seen{0};
    std::uint64_t non_file_descriptors_skipped{0};
    std::uint64_t faces_emitted{0};
    std::uint64_t duplicate_faces_skipped{0};
    std::uint64_t coverage_codepoints{0};
    std::uint64_t coverage_ranges{0};
    std::uint64_t variable_faces{0};
    std::uint64_t color_faces{0};
    std::uint64_t monospace_faces{0};
    FontDiscoveryStats generation;
};

const char* coretext_discovery_error_kind_name(
    CoreTextDiscoveryErrorKind kind) noexcept;

// Enumerates physical file-backed faces visible to CoreText and immediately
// builds one immutable core generation. No CF/CT object or borrowed buffer
// escapes this call.
bool build_coretext_generation(
    std::uint64_t generation_id,
    std::size_t discovery_hard_limit,
    std::size_t catalog_hard_limit,
    std::shared_ptr<const FontCatalogGeneration>* output,
    CoreTextDiscoveryStats* stats,
    CoreTextDiscoveryError* error) noexcept;

} // namespace zevryon::text
