#pragma once

#include "catalog_harfbuzz_shaper.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zevryon::text {

class PreparedHarfBuzzFace final {
public:
    ~PreparedHarfBuzzFace();

    PreparedHarfBuzzFace(const PreparedHarfBuzzFace&) = delete;
    PreparedHarfBuzzFace& operator=(const PreparedHarfBuzzFace&) = delete;
    PreparedHarfBuzzFace(PreparedHarfBuzzFace&&) = delete;
    PreparedHarfBuzzFace& operator=(PreparedHarfBuzzFace&&) = delete;

    bool valid() const noexcept;
    std::uint64_t generation_id() const noexcept;
    FontFaceId face_id() const noexcept;
    std::uint64_t resource_id() const noexcept;
    std::size_t font_bytes() const noexcept;
    std::uint32_t glyph_count() const noexcept;
    std::uint32_t units_per_em() const noexcept;
    const CatalogFontFaceBinding& binding() const noexcept;

private:
    struct NativeState;

    PreparedHarfBuzzFace(
        CatalogFontFaceBinding binding,
        std::unique_ptr<NativeState> native,
        std::uint32_t glyph_count,
        std::uint32_t units_per_em) noexcept;

    friend bool prepare_harfbuzz_face(
        const CatalogFontFaceBinding&,
        std::shared_ptr<const PreparedHarfBuzzFace>*,
        struct PreparedHarfBuzzFaceStats*,
        struct PreparedHarfBuzzFaceError*) noexcept;
    friend void* prepared_harfbuzz_native_face(
        const PreparedHarfBuzzFace&) noexcept;

    CatalogFontFaceBinding binding_;
    std::unique_ptr<NativeState> native_;
    std::uint32_t glyph_count_{0};
    std::uint32_t units_per_em_{0};
};

enum class PreparedHarfBuzzFaceErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    InvalidBinding,
    BlobCreationFailed,
    FaceCreationFailed,
    InvalidFontData,
    AllocationFailed
};

struct PreparedHarfBuzzFaceError {
    PreparedHarfBuzzFaceErrorKind kind{PreparedHarfBuzzFaceErrorKind::None};
    std::string message;
};

struct PreparedHarfBuzzFaceStats {
    std::uint64_t generation_id{0};
    FontFaceId face_id{kInvalidFontFaceId};
    std::uint64_t resource_id{0};
    std::size_t font_bytes{0};
    std::uint32_t glyph_count{0};
    std::uint32_t units_per_em{0};
    bool blob_created{false};
    bool face_created{false};
    bool face_immutable{false};
};

const char* prepared_harfbuzz_face_error_kind_name(
    PreparedHarfBuzzFaceErrorKind kind) noexcept;

// Creates one HarfBuzz blob and immutable face from an already verified catalog
// binding. The output retains the binding, so source bytes remain alive even
// after external generation ownership is released and the resource cache is
// cleared. A failure publishes no prepared face.
bool prepare_harfbuzz_face(
    const CatalogFontFaceBinding& binding,
    std::shared_ptr<const PreparedHarfBuzzFace>* output,
    PreparedHarfBuzzFaceStats* stats,
    PreparedHarfBuzzFaceError* error) noexcept;

// Internal bridge for the prepared-face shaping backend. The returned pointer
// is borrowed and remains valid only while the PreparedHarfBuzzFace is alive.
void* prepared_harfbuzz_native_face(
    const PreparedHarfBuzzFace& prepared) noexcept;

} // namespace zevryon::text
