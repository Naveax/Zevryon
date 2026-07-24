#pragma once

#include "font_discovery_generation.hpp"
#include "font_file_loader.hpp"
#include "font_load_locator.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zevryon::text {

enum class CatalogFontResourceErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    InvalidFaceId,
    IdentityParseFailed,
    FontconfigSysrootUnsupported,
    MultiFileUnsupported,
    FaceIndexUnresolved,
    PathTooLong,
    PathConversionFailed,
    FileLoadFailed
};

struct CatalogFontResourceError {
    CatalogFontResourceErrorKind kind{CatalogFontResourceErrorKind::None};
    FontLoadLocatorError locator_error;
    FontFileLoadError file_error;
    std::string message;
};

struct CatalogFontResourceStats {
    std::uint64_t generation_id{0};
    FontFaceId face_id{kInvalidFontFaceId};
    std::uint64_t identity_bytes{0};
    std::uint64_t path_bytes{0};
    FontPlatformIdentityKind platform_kind{
        FontPlatformIdentityKind::Fontconfig};
    FontLoadCapability capability{
        FontLoadCapability::SingleFileWithFaceIndex};
    FontLoadLocatorStats locator;
    FontFileLoadStats file_load;
};

const char* catalog_font_resource_error_kind_name(
    CatalogFontResourceErrorKind kind) noexcept;

// Resolves one face from an immutable catalog generation through its platform
// identity and the bounded stable file loader. The generation is retained for
// the complete call. Output is published only after all stages succeed.
bool resolve_catalog_font_resource(
    std::shared_ptr<const FontCatalogGeneration> generation,
    FontFaceId face_id,
    std::size_t staging_hard_limit,
    VerifiedFontResourceCache* cache,
    std::shared_ptr<const VerifiedFontResource>* output,
    CatalogFontResourceStats* stats,
    CatalogFontResourceError* error) noexcept;

} // namespace zevryon::text
