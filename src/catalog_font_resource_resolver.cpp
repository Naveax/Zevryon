#include "catalog_font_resource_resolver.hpp"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace zevryon::text {
namespace {

constexpr std::size_t kMaximumPortablePathBytes = 32768U;

void clear_error(CatalogFontResourceError* error) noexcept {
    if (error != nullptr) {
        error->kind = CatalogFontResourceErrorKind::None;
        error->locator_error = {};
        error->file_error = {};
        error->message.clear();
    }
}

bool fail(
    CatalogFontResourceErrorKind kind,
    const char* message,
    CatalogFontResourceError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool path_from_utf8(
    std::string_view utf8,
    std::filesystem::path* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    *output = {};
    try {
        std::u8string encoded;
        encoded.resize(utf8.size());
        if (!utf8.empty()) {
            std::memcpy(encoded.data(), utf8.data(), utf8.size());
        }
        *output = std::filesystem::path(encoded);
        return true;
    } catch (...) {
        *output = {};
        return false;
    }
}

} // namespace

const char* catalog_font_resource_error_kind_name(
    CatalogFontResourceErrorKind kind) noexcept {
    switch (kind) {
    case CatalogFontResourceErrorKind::None:
        return "none";
    case CatalogFontResourceErrorKind::InvalidArgument:
        return "invalid_argument";
    case CatalogFontResourceErrorKind::InvalidFaceId:
        return "invalid_face_id";
    case CatalogFontResourceErrorKind::IdentityParseFailed:
        return "identity_parse_failed";
    case CatalogFontResourceErrorKind::FontconfigSysrootUnsupported:
        return "fontconfig_sysroot_unsupported";
    case CatalogFontResourceErrorKind::MultiFileUnsupported:
        return "multi_file_unsupported";
    case CatalogFontResourceErrorKind::FaceIndexUnresolved:
        return "face_index_unresolved";
    case CatalogFontResourceErrorKind::PathTooLong:
        return "path_too_long";
    case CatalogFontResourceErrorKind::PathConversionFailed:
        return "path_conversion_failed";
    case CatalogFontResourceErrorKind::FileLoadFailed:
        return "file_load_failed";
    }
    return "unknown";
}

bool resolve_catalog_font_resource(
    std::shared_ptr<const FontCatalogGeneration> generation,
    FontFaceId face_id,
    std::size_t staging_hard_limit,
    VerifiedFontResourceCache* cache,
    std::shared_ptr<const VerifiedFontResource>* output,
    CatalogFontResourceStats* stats,
    CatalogFontResourceError* error) noexcept {
    if (output != nullptr) {
        output->reset();
    }
    if (stats != nullptr) {
        *stats = {};
    }
    clear_error(error);

    if (!generation || cache == nullptr || output == nullptr ||
        stats == nullptr || error == nullptr || staging_hard_limit == 0U) {
        return fail(
            CatalogFontResourceErrorKind::InvalidArgument,
            "generation, cache, output, stats, error, and staging limit are required",
            error);
    }

    stats->generation_id = generation->generation_id();
    stats->face_id = face_id;
    if (face_id == kInvalidFontFaceId ||
        static_cast<std::size_t>(face_id) >=
            generation->discovery_records().size()) {
        return fail(
            CatalogFontResourceErrorKind::InvalidFaceId,
            "font face id is outside the retained generation",
            error);
    }

    const std::string_view identity = generation->identity(face_id);
    stats->identity_bytes = identity.size();
    FontLoadLocator locator;
    FontLoadLocatorError locator_error;
    if (!parse_font_load_locator(
            identity,
            &locator,
            &stats->locator,
            &locator_error)) {
        error->kind = CatalogFontResourceErrorKind::IdentityParseFailed;
        error->locator_error = locator_error;
        try {
            error->message = "catalog platform identity could not be parsed: ";
            error->message +=
                font_load_locator_error_kind_name(locator_error.kind);
        } catch (...) {
            error->message.clear();
        }
        return false;
    }

    stats->platform_kind = locator.kind;
    stats->capability = locator.capability;
    if (locator.kind == FontPlatformIdentityKind::Fontconfig &&
        !locator.sysroot.empty()) {
        return fail(
            CatalogFontResourceErrorKind::FontconfigSysrootUnsupported,
            "portable resolver does not join non-empty Fontconfig sysroots",
            error);
    }
    if (locator.capability == FontLoadCapability::MultiFile) {
        return fail(
            CatalogFontResourceErrorKind::MultiFileUnsupported,
            "portable resolver cannot load a multi-file DirectWrite face",
            error);
    }
    if (locator.capability ==
            FontLoadCapability::SingleFileFaceIndexUnresolved ||
        !locator.has_face_index) {
        return fail(
            CatalogFontResourceErrorKind::FaceIndexUnresolved,
            "platform identity does not expose a resolvable sfnt face index",
            error);
    }
    if (!locator.directly_loadable()) {
        return fail(
            CatalogFontResourceErrorKind::InvalidArgument,
            "platform identity is not directly loadable",
            error);
    }

    stats->path_bytes = locator.file_path.size();
    if (locator.file_path.size() > kMaximumPortablePathBytes) {
        return fail(
            CatalogFontResourceErrorKind::PathTooLong,
            "platform font path exceeds the portable resolver limit",
            error);
    }

    std::filesystem::path path;
    if (!path_from_utf8(locator.file_path, &path)) {
        return fail(
            CatalogFontResourceErrorKind::PathConversionFailed,
            "UTF-8 platform font path could not be converted",
            error);
    }

    std::shared_ptr<const VerifiedFontResource> candidate;
    FontFileLoadError file_error;
    if (!load_verified_font_file(
            path,
            locator.face_index,
            staging_hard_limit,
            cache,
            &candidate,
            &stats->file_load,
            &file_error)) {
        error->kind = CatalogFontResourceErrorKind::FileLoadFailed;
        error->file_error = std::move(file_error);
        try {
            error->message = "catalog font file load failed: ";
            error->message +=
                font_file_load_error_kind_name(error->file_error.kind);
        } catch (...) {
            error->message.clear();
        }
        return false;
    }

    *output = std::move(candidate);
    return true;
}

} // namespace zevryon::text
