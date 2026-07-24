#include "prepared_harfbuzz_face.hpp"

#include <hb.h>

#include <climits>
#include <memory>
#include <new>
#include <utility>

namespace zevryon::text {
namespace {

void clear_error(PreparedHarfBuzzFaceError* error) noexcept {
    if (error != nullptr) {
        error->kind = PreparedHarfBuzzFaceErrorKind::None;
        error->message.clear();
    }
}

bool fail(
    PreparedHarfBuzzFaceErrorKind kind,
    const char* message,
    PreparedHarfBuzzFaceError* error) noexcept {
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

} // namespace

struct PreparedHarfBuzzFace::NativeState {
    hb_blob_t* blob{nullptr};
    hb_face_t* face{nullptr};

    ~NativeState() {
        if (face != nullptr) {
            hb_face_destroy(face);
        }
        if (blob != nullptr) {
            hb_blob_destroy(blob);
        }
    }
};

PreparedHarfBuzzFace::PreparedHarfBuzzFace(
    CatalogFontFaceBinding binding,
    std::unique_ptr<NativeState> native,
    std::uint32_t glyph_count,
    std::uint32_t units_per_em) noexcept
    : binding_(std::move(binding)),
      native_(std::move(native)),
      glyph_count_(glyph_count),
      units_per_em_(units_per_em) {}

PreparedHarfBuzzFace::~PreparedHarfBuzzFace() = default;

bool PreparedHarfBuzzFace::valid() const noexcept {
    return binding_.valid() && native_ != nullptr &&
        native_->blob != nullptr && native_->face != nullptr &&
        glyph_count_ != 0U && units_per_em_ != 0U;
}

std::uint64_t PreparedHarfBuzzFace::generation_id() const noexcept {
    return binding_.generation_id();
}

FontFaceId PreparedHarfBuzzFace::face_id() const noexcept {
    return binding_.face_id();
}

std::uint64_t PreparedHarfBuzzFace::resource_id() const noexcept {
    return binding_.resource_id();
}

std::size_t PreparedHarfBuzzFace::font_bytes() const noexcept {
    return binding_.resource() != nullptr
        ? binding_.resource()->bytes().size()
        : 0U;
}

std::uint32_t PreparedHarfBuzzFace::glyph_count() const noexcept {
    return glyph_count_;
}

std::uint32_t PreparedHarfBuzzFace::units_per_em() const noexcept {
    return units_per_em_;
}

const CatalogFontFaceBinding& PreparedHarfBuzzFace::binding() const noexcept {
    return binding_;
}

const char* prepared_harfbuzz_face_error_kind_name(
    PreparedHarfBuzzFaceErrorKind kind) noexcept {
    switch (kind) {
    case PreparedHarfBuzzFaceErrorKind::None:
        return "none";
    case PreparedHarfBuzzFaceErrorKind::InvalidArgument:
        return "invalid_argument";
    case PreparedHarfBuzzFaceErrorKind::InvalidBinding:
        return "invalid_binding";
    case PreparedHarfBuzzFaceErrorKind::BlobCreationFailed:
        return "blob_creation_failed";
    case PreparedHarfBuzzFaceErrorKind::FaceCreationFailed:
        return "face_creation_failed";
    case PreparedHarfBuzzFaceErrorKind::InvalidFontData:
        return "invalid_font_data";
    case PreparedHarfBuzzFaceErrorKind::AllocationFailed:
        return "allocation_failed";
    }
    return "unknown";
}

bool prepare_harfbuzz_face(
    const CatalogFontFaceBinding& binding,
    std::shared_ptr<const PreparedHarfBuzzFace>* output,
    PreparedHarfBuzzFaceStats* stats,
    PreparedHarfBuzzFaceError* error) noexcept {
    if (output != nullptr) {
        output->reset();
    }
    if (stats != nullptr) {
        *stats = {};
    }
    clear_error(error);

    if (output == nullptr || stats == nullptr || error == nullptr) {
        return fail(
            PreparedHarfBuzzFaceErrorKind::InvalidArgument,
            "output, stats, and error are required",
            error);
    }
    if (!binding.valid()) {
        return fail(
            PreparedHarfBuzzFaceErrorKind::InvalidBinding,
            "catalog font-face binding is not valid",
            error);
    }

    const std::shared_ptr<const VerifiedFontResource> resource =
        binding.resource();
    if (!resource || resource->bytes().empty() ||
        resource->bytes().size() > static_cast<std::size_t>(UINT_MAX)) {
        return fail(
            PreparedHarfBuzzFaceErrorKind::InvalidBinding,
            "binding does not expose HarfBuzz-compatible verified bytes",
            error);
    }

    stats->generation_id = binding.generation_id();
    stats->face_id = binding.face_id();
    stats->resource_id = binding.resource_id();
    stats->font_bytes = resource->bytes().size();

    try {
        auto native = std::make_unique<PreparedHarfBuzzFace::NativeState>();
        native->blob = hb_blob_create(
            reinterpret_cast<const char*>(resource->bytes().data()),
            static_cast<unsigned int>(resource->bytes().size()),
            HB_MEMORY_MODE_READONLY,
            nullptr,
            nullptr);
        if (native->blob == nullptr ||
            static_cast<std::size_t>(hb_blob_get_length(native->blob)) !=
                resource->bytes().size()) {
            return fail(
                PreparedHarfBuzzFaceErrorKind::BlobCreationFailed,
                "HarfBuzz failed to create a read-only verified-resource blob",
                error);
        }
        stats->blob_created = true;

        native->face = hb_face_create(
            native->blob,
            resource->view().face_index());
        if (native->face == nullptr) {
            return fail(
                PreparedHarfBuzzFaceErrorKind::FaceCreationFailed,
                "HarfBuzz failed to create the selected verified face",
                error);
        }
        stats->face_created = true;

        const unsigned int glyph_count = hb_face_get_glyph_count(native->face);
        const unsigned int units_per_em = hb_face_get_upem(native->face);
        if (glyph_count == 0U || units_per_em == 0U ||
            units_per_em > static_cast<unsigned int>(INT_MAX)) {
            return fail(
                PreparedHarfBuzzFaceErrorKind::InvalidFontData,
                "prepared HarfBuzz face has invalid glyph or units-per-em metadata",
                error);
        }

        hb_face_make_immutable(native->face);
        stats->glyph_count = glyph_count;
        stats->units_per_em = units_per_em;
        stats->face_immutable = true;

        CatalogFontFaceBinding retained_binding = binding;
        std::shared_ptr<const PreparedHarfBuzzFace> candidate(
            new PreparedHarfBuzzFace(
                std::move(retained_binding),
                std::move(native),
                glyph_count,
                units_per_em));
        if (!candidate->valid()) {
            return fail(
                PreparedHarfBuzzFaceErrorKind::InvalidFontData,
                "prepared HarfBuzz face failed final ownership validation",
                error);
        }
        *output = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            PreparedHarfBuzzFaceErrorKind::AllocationFailed,
            "prepared HarfBuzz face allocation failed",
            error);
    } catch (...) {
        return fail(
            PreparedHarfBuzzFaceErrorKind::AllocationFailed,
            "prepared HarfBuzz face construction failed",
            error);
    }
}

void* prepared_harfbuzz_native_face(
    const PreparedHarfBuzzFace& prepared) noexcept {
    return prepared.native_ != nullptr
        ? static_cast<void*>(prepared.native_->face)
        : nullptr;
}

} // namespace zevryon::text
