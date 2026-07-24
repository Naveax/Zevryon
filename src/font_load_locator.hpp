#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zevryon::text {

enum class FontPlatformIdentityKind : std::uint8_t {
    Fontconfig = 0,
    DirectWrite,
    CoreText
};

enum class FontLoadCapability : std::uint8_t {
    SingleFileWithFaceIndex = 0,
    MultiFile,
    SingleFileFaceIndexUnresolved
};

enum class FontLoadLocatorErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    UnsupportedPrefix,
    MissingLength,
    NonCanonicalDecimal,
    DecimalOverflow,
    MissingLengthSeparator,
    TruncatedField,
    MissingFieldTerminator,
    EmptyRequiredField,
    EmbeddedNull,
    InvalidFieldCount,
    InvalidNumericValue,
    TooManyFields,
    TrailingData
};

struct FontLoadLocatorError {
    FontLoadLocatorErrorKind kind{FontLoadLocatorErrorKind::None};
    std::size_t byte_offset{0};
    std::uint32_t field_index{0};
    const char* message{nullptr};
};

struct FontLoadLocatorStats {
    std::uint64_t identity_bytes{0};
    std::uint32_t fields_parsed{0};
    std::uint32_t file_count{0};
    std::uint32_t variation_count{0};
};

struct FontLoadLocator {
    FontPlatformIdentityKind kind{FontPlatformIdentityKind::Fontconfig};
    FontLoadCapability capability{
        FontLoadCapability::SingleFileWithFaceIndex};
    std::string_view sysroot;
    std::string_view file_path;
    std::string_view postscript_name;
    std::string_view variation_descriptor;
    std::uint32_t face_index{0};
    std::uint32_t file_count{0};
    bool has_face_index{false};

    bool directly_loadable() const noexcept {
        return capability == FontLoadCapability::SingleFileWithFaceIndex &&
               has_face_index && file_count == 1U && !file_path.empty();
    }
};

const char* font_platform_identity_kind_name(
    FontPlatformIdentityKind kind) noexcept;
const char* font_load_capability_name(FontLoadCapability capability) noexcept;
const char* font_load_locator_error_kind_name(
    FontLoadLocatorErrorKind kind) noexcept;

// Parses one immutable platform identity without allocation. All string views in
// the output refer to `identity`; the caller must keep it alive and immutable.
// Failure always resets output and stats.
bool parse_font_load_locator(
    std::string_view identity,
    FontLoadLocator* output,
    FontLoadLocatorStats* stats,
    FontLoadLocatorError* error) noexcept;

} // namespace zevryon::text
