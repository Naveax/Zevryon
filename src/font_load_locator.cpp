#include "font_load_locator.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace zevryon::text {
namespace {

constexpr std::string_view kFontconfigPrefix = "fontconfig|";
constexpr std::string_view kDirectWritePrefix = "directwrite|";
constexpr std::string_view kCoreTextPrefix = "coretext|";
constexpr std::uint32_t kMaximumIdentityFields = 4096U;
constexpr std::uint32_t kMaximumVariationCoordinates = 2048U;

struct ParsedField {
    std::string_view value;
    std::size_t byte_offset{0};
    std::uint32_t index{0};
};

void clear_error(FontLoadLocatorError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    FontLoadLocatorErrorKind kind,
    std::size_t byte_offset,
    std::uint32_t field_index,
    const char* message,
    FontLoadLocatorError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->byte_offset = byte_offset;
        error->field_index = field_index;
        error->message = message;
    }
    return false;
}

bool has_embedded_null(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

class FieldReader final {
public:
    FieldReader(
        std::string_view identity,
        std::size_t position,
        FontLoadLocatorStats* stats,
        FontLoadLocatorError* error) noexcept
        : identity_(identity),
          position_(position),
          stats_(stats),
          error_(error) {}

    bool next(ParsedField* output) noexcept {
        if (output == nullptr) {
            return fail(
                FontLoadLocatorErrorKind::InvalidArgument,
                position_,
                field_index_,
                "field output is required",
                error_);
        }
        *output = {};
        if (field_index_ >= kMaximumIdentityFields) {
            return fail(
                FontLoadLocatorErrorKind::TooManyFields,
                position_,
                field_index_,
                "platform identity exceeds the field-count limit",
                error_);
        }
        if (position_ >= identity_.size()) {
            return fail(
                FontLoadLocatorErrorKind::TruncatedField,
                position_,
                field_index_,
                "platform identity ended before the next field",
                error_);
        }

        const std::size_t length_offset = position_;
        std::size_t separator = position_;
        while (separator < identity_.size() && identity_[separator] != ':') {
            const char value = identity_[separator];
            if (value < '0' || value > '9') {
                return fail(
                    FontLoadLocatorErrorKind::NonCanonicalDecimal,
                    separator,
                    field_index_,
                    "field length must contain only decimal digits",
                    error_);
            }
            ++separator;
        }
        if (separator == position_) {
            return fail(
                FontLoadLocatorErrorKind::MissingLength,
                length_offset,
                field_index_,
                "field length is empty",
                error_);
        }
        if (separator == identity_.size()) {
            return fail(
                FontLoadLocatorErrorKind::MissingLengthSeparator,
                separator,
                field_index_,
                "field length is not followed by a colon",
                error_);
        }
        if (separator - position_ > 1U && identity_[position_] == '0') {
            return fail(
                FontLoadLocatorErrorKind::NonCanonicalDecimal,
                position_,
                field_index_,
                "field length contains a leading zero",
                error_);
        }

        std::size_t length = 0U;
        for (std::size_t index = position_; index < separator; ++index) {
            const std::size_t digit =
                static_cast<std::size_t>(identity_[index] - '0');
            if (length >
                (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
                return fail(
                    FontLoadLocatorErrorKind::DecimalOverflow,
                    length_offset,
                    field_index_,
                    "field length overflows size_t",
                    error_);
            }
            length = length * 10U + digit;
        }

        const std::size_t payload_offset = separator + 1U;
        const std::size_t remaining = identity_.size() - payload_offset;
        if (length > remaining || remaining - length < 1U) {
            return fail(
                FontLoadLocatorErrorKind::TruncatedField,
                payload_offset,
                field_index_,
                "field payload exceeds the remaining identity bytes",
                error_);
        }
        const std::size_t terminator = payload_offset + length;
        if (identity_[terminator] != '|') {
            return fail(
                FontLoadLocatorErrorKind::MissingFieldTerminator,
                terminator,
                field_index_,
                "field payload is not followed by a pipe",
                error_);
        }

        const std::string_view value =
            identity_.substr(payload_offset, length);
        if (has_embedded_null(value)) {
            return fail(
                FontLoadLocatorErrorKind::EmbeddedNull,
                payload_offset,
                field_index_,
                "identity fields may not contain embedded null bytes",
                error_);
        }

        output->value = value;
        output->byte_offset = payload_offset;
        output->index = field_index_;
        position_ = terminator + 1U;
        ++field_index_;
        if (stats_ != nullptr) {
            stats_->fields_parsed = field_index_;
        }
        return true;
    }

    bool finished() noexcept {
        if (position_ != identity_.size()) {
            return fail(
                FontLoadLocatorErrorKind::TrailingData,
                position_,
                field_index_,
                "platform identity contains trailing bytes",
                error_);
        }
        return true;
    }

private:
    std::string_view identity_;
    std::size_t position_{0};
    std::uint32_t field_index_{0};
    FontLoadLocatorStats* stats_{nullptr};
    FontLoadLocatorError* error_{nullptr};
};

bool require_nonempty(
    const ParsedField& field,
    const char* message,
    FontLoadLocatorError* error) noexcept {
    return !field.value.empty() || fail(
        FontLoadLocatorErrorKind::EmptyRequiredField,
        field.byte_offset,
        field.index,
        message,
        error);
}

bool parse_unsigned(
    const ParsedField& field,
    std::uint64_t maximum,
    std::uint64_t* output,
    FontLoadLocatorError* error) noexcept {
    if (output == nullptr || field.value.empty()) {
        return fail(
            FontLoadLocatorErrorKind::InvalidNumericValue,
            field.byte_offset,
            field.index,
            "numeric identity field is empty",
            error);
    }
    if (field.value.size() > 1U && field.value.front() == '0') {
        return fail(
            FontLoadLocatorErrorKind::NonCanonicalDecimal,
            field.byte_offset,
            field.index,
            "numeric identity field contains a leading zero",
            error);
    }

    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < field.value.size(); ++index) {
        const char raw = field.value[index];
        if (raw < '0' || raw > '9') {
            return fail(
                FontLoadLocatorErrorKind::InvalidNumericValue,
                field.byte_offset + index,
                field.index,
                "numeric identity field contains a non-digit",
                error);
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(raw - '0');
        if (value > (maximum - digit) / 10U) {
            return fail(
                FontLoadLocatorErrorKind::DecimalOverflow,
                field.byte_offset,
                field.index,
                "numeric identity field exceeds its contract",
                error);
        }
        value = value * 10U + digit;
    }
    *output = value;
    return true;
}

bool validate_signed_64(
    const ParsedField& field,
    FontLoadLocatorError* error) noexcept {
    if (field.value.empty()) {
        return fail(
            FontLoadLocatorErrorKind::InvalidNumericValue,
            field.byte_offset,
            field.index,
            "signed identity field is empty",
            error);
    }

    const bool negative = field.value.front() == '-';
    const std::size_t digits_offset = negative ? 1U : 0U;
    if (digits_offset == field.value.size()) {
        return fail(
            FontLoadLocatorErrorKind::InvalidNumericValue,
            field.byte_offset,
            field.index,
            "signed identity field has no digits",
            error);
    }
    if (field.value.front() == '+') {
        return fail(
            FontLoadLocatorErrorKind::NonCanonicalDecimal,
            field.byte_offset,
            field.index,
            "signed identity field may not contain a plus sign",
            error);
    }
    if (field.value.size() - digits_offset > 1U &&
        field.value[digits_offset] == '0') {
        return fail(
            FontLoadLocatorErrorKind::NonCanonicalDecimal,
            field.byte_offset + digits_offset,
            field.index,
            "signed identity field contains a leading zero",
            error);
    }
    if (negative && field.value.size() == 2U && field.value[1] == '0') {
        return fail(
            FontLoadLocatorErrorKind::NonCanonicalDecimal,
            field.byte_offset,
            field.index,
            "negative zero is not canonical",
            error);
    }

    const std::uint64_t maximum = negative
        ? static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()) + 1U
        : static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max());
    std::uint64_t value = 0U;
    for (std::size_t index = digits_offset;
         index < field.value.size();
         ++index) {
        const char raw = field.value[index];
        if (raw < '0' || raw > '9') {
            return fail(
                FontLoadLocatorErrorKind::InvalidNumericValue,
                field.byte_offset + index,
                field.index,
                "signed identity field contains a non-digit",
                error);
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(raw - '0');
        if (value > (maximum - digit) / 10U) {
            return fail(
                FontLoadLocatorErrorKind::DecimalOverflow,
                field.byte_offset,
                field.index,
                "signed identity field exceeds int64",
                error);
        }
        value = value * 10U + digit;
    }
    return true;
}

bool parse_u32(
    const ParsedField& field,
    std::uint32_t* output,
    FontLoadLocatorError* error) noexcept {
    std::uint64_t value = 0U;
    if (!parse_unsigned(
            field,
            std::numeric_limits<std::uint32_t>::max(),
            &value,
            error)) {
        return false;
    }
    *output = static_cast<std::uint32_t>(value);
    return true;
}

bool parse_fontconfig(
    std::string_view identity,
    FontLoadLocator* output,
    FontLoadLocatorStats* stats,
    FontLoadLocatorError* error) noexcept {
    FieldReader fields(identity, kFontconfigPrefix.size(), stats, error);
    ParsedField sysroot;
    ParsedField file;
    ParsedField face;
    ParsedField postscript;
    ParsedField variations;
    if (!fields.next(&sysroot) ||
        !fields.next(&file) ||
        !require_nonempty(file, "Fontconfig file path is empty", error) ||
        !fields.next(&face) ||
        !parse_u32(face, &output->face_index, error) ||
        !fields.next(&postscript) ||
        !fields.next(&variations) ||
        !fields.finished()) {
        return false;
    }

    output->kind = FontPlatformIdentityKind::Fontconfig;
    output->capability = FontLoadCapability::SingleFileWithFaceIndex;
    output->sysroot = sysroot.value;
    output->file_path = file.value;
    output->postscript_name = postscript.value;
    output->variation_descriptor = variations.value;
    output->file_count = 1U;
    output->has_face_index = true;
    if (stats != nullptr) {
        stats->file_count = 1U;
    }
    return true;
}

bool parse_directwrite(
    std::string_view identity,
    FontLoadLocator* output,
    FontLoadLocatorStats* stats,
    FontLoadLocatorError* error) noexcept {
    FieldReader fields(identity, kDirectWritePrefix.size(), stats, error);
    ParsedField count_field;
    std::uint32_t file_count = 0U;
    if (!fields.next(&count_field) ||
        !parse_u32(count_field, &file_count, error)) {
        return false;
    }
    if (file_count == 0U) {
        return fail(
            FontLoadLocatorErrorKind::InvalidFieldCount,
            count_field.byte_offset,
            count_field.index,
            "DirectWrite identity contains no font files",
            error);
    }
    if (file_count > kMaximumIdentityFields - 7U) {
        return fail(
            FontLoadLocatorErrorKind::TooManyFields,
            count_field.byte_offset,
            count_field.index,
            "DirectWrite file count exceeds the identity field limit",
            error);
    }

    std::string_view single_path;
    for (std::uint32_t index = 0U; index < file_count; ++index) {
        ParsedField path;
        if (!fields.next(&path) ||
            !require_nonempty(path, "DirectWrite file path is empty", error)) {
            return false;
        }
        if (file_count == 1U) {
            single_path = path.value;
        }
    }

    ParsedField face_type;
    ParsedField face_index;
    ParsedField weight;
    ParsedField stretch;
    ParsedField style;
    ParsedField postscript;
    std::uint32_t ignored = 0U;
    if (!fields.next(&face_type) || !parse_u32(face_type, &ignored, error) ||
        !fields.next(&face_index) ||
        !parse_u32(face_index, &output->face_index, error) ||
        !fields.next(&weight) || !parse_u32(weight, &ignored, error) ||
        !fields.next(&stretch) || !parse_u32(stretch, &ignored, error) ||
        !fields.next(&style) || !parse_u32(style, &ignored, error) ||
        !fields.next(&postscript) || !fields.finished()) {
        return false;
    }

    output->kind = FontPlatformIdentityKind::DirectWrite;
    output->capability = file_count == 1U
        ? FontLoadCapability::SingleFileWithFaceIndex
        : FontLoadCapability::MultiFile;
    output->file_path = single_path;
    output->postscript_name = postscript.value;
    output->file_count = file_count;
    output->has_face_index = true;
    if (stats != nullptr) {
        stats->file_count = file_count;
    }
    return true;
}

bool parse_coretext(
    std::string_view identity,
    FontLoadLocator* output,
    FontLoadLocatorStats* stats,
    FontLoadLocatorError* error) noexcept {
    FieldReader fields(identity, kCoreTextPrefix.size(), stats, error);
    ParsedField path;
    ParsedField postscript;
    ParsedField weight_field;
    ParsedField width_field;
    ParsedField slant_field;
    ParsedField variation_count_field;
    if (!fields.next(&path) ||
        !require_nonempty(path, "CoreText file path is empty", error) ||
        !fields.next(&postscript) ||
        !require_nonempty(postscript, "CoreText PostScript name is empty", error) ||
        !fields.next(&weight_field) ||
        !fields.next(&width_field) ||
        !fields.next(&slant_field) ||
        !fields.next(&variation_count_field)) {
        return false;
    }

    std::uint32_t weight = 0U;
    std::uint32_t width = 0U;
    std::uint32_t slant = 0U;
    std::uint32_t variation_count = 0U;
    if (!parse_u32(weight_field, &weight, error) ||
        !parse_u32(width_field, &width, error) ||
        !parse_u32(slant_field, &slant, error) ||
        !parse_u32(variation_count_field, &variation_count, error)) {
        return false;
    }
    if (weight < 1U || weight > 1000U ||
        width < 1U || width > 9U || slant > 2U) {
        return fail(
            FontLoadLocatorErrorKind::InvalidNumericValue,
            weight_field.byte_offset,
            weight_field.index,
            "CoreText weight, width, or slant is outside the discovery contract",
            error);
    }
    if (variation_count > kMaximumVariationCoordinates) {
        return fail(
            FontLoadLocatorErrorKind::TooManyFields,
            variation_count_field.byte_offset,
            variation_count_field.index,
            "CoreText variation count exceeds the parser limit",
            error);
    }

    for (std::uint32_t index = 0U; index < variation_count; ++index) {
        ParsedField identifier;
        ParsedField value_bits;
        std::uint64_t ignored = 0U;
        if (!fields.next(&identifier) ||
            !validate_signed_64(identifier, error) ||
            !fields.next(&value_bits) ||
            !parse_unsigned(
                value_bits,
                std::numeric_limits<std::uint64_t>::max(),
                &ignored,
                error)) {
            return false;
        }
    }
    if (!fields.finished()) {
        return false;
    }

    output->kind = FontPlatformIdentityKind::CoreText;
    output->capability = FontLoadCapability::SingleFileFaceIndexUnresolved;
    output->file_path = path.value;
    output->postscript_name = postscript.value;
    output->file_count = 1U;
    output->face_index = 0U;
    output->has_face_index = false;
    if (stats != nullptr) {
        stats->file_count = 1U;
        stats->variation_count = variation_count;
    }
    return true;
}

} // namespace

const char* font_platform_identity_kind_name(
    FontPlatformIdentityKind kind) noexcept {
    switch (kind) {
    case FontPlatformIdentityKind::Fontconfig:
        return "fontconfig";
    case FontPlatformIdentityKind::DirectWrite:
        return "directwrite";
    case FontPlatformIdentityKind::CoreText:
        return "coretext";
    }
    return "unknown";
}

const char* font_load_capability_name(FontLoadCapability capability) noexcept {
    switch (capability) {
    case FontLoadCapability::SingleFileWithFaceIndex:
        return "single_file_with_face_index";
    case FontLoadCapability::MultiFile:
        return "multi_file";
    case FontLoadCapability::SingleFileFaceIndexUnresolved:
        return "single_file_face_index_unresolved";
    }
    return "unknown";
}

const char* font_load_locator_error_kind_name(
    FontLoadLocatorErrorKind kind) noexcept {
    switch (kind) {
    case FontLoadLocatorErrorKind::None:
        return "none";
    case FontLoadLocatorErrorKind::InvalidArgument:
        return "invalid_argument";
    case FontLoadLocatorErrorKind::UnsupportedPrefix:
        return "unsupported_prefix";
    case FontLoadLocatorErrorKind::MissingLength:
        return "missing_length";
    case FontLoadLocatorErrorKind::NonCanonicalDecimal:
        return "non_canonical_decimal";
    case FontLoadLocatorErrorKind::DecimalOverflow:
        return "decimal_overflow";
    case FontLoadLocatorErrorKind::MissingLengthSeparator:
        return "missing_length_separator";
    case FontLoadLocatorErrorKind::TruncatedField:
        return "truncated_field";
    case FontLoadLocatorErrorKind::MissingFieldTerminator:
        return "missing_field_terminator";
    case FontLoadLocatorErrorKind::EmptyRequiredField:
        return "empty_required_field";
    case FontLoadLocatorErrorKind::EmbeddedNull:
        return "embedded_null";
    case FontLoadLocatorErrorKind::InvalidFieldCount:
        return "invalid_field_count";
    case FontLoadLocatorErrorKind::InvalidNumericValue:
        return "invalid_numeric_value";
    case FontLoadLocatorErrorKind::TooManyFields:
        return "too_many_fields";
    case FontLoadLocatorErrorKind::TrailingData:
        return "trailing_data";
    }
    return "unknown";
}

bool parse_font_load_locator(
    std::string_view identity,
    FontLoadLocator* output,
    FontLoadLocatorStats* stats,
    FontLoadLocatorError* error) noexcept {
    if (output != nullptr) {
        *output = {};
    }
    if (stats != nullptr) {
        *stats = {};
        stats->identity_bytes = identity.size();
    }
    clear_error(error);

    if (output == nullptr || identity.empty()) {
        return fail(
            FontLoadLocatorErrorKind::InvalidArgument,
            0U,
            0U,
            "identity and output are required",
            error);
    }

    bool parsed = false;
    if (identity.starts_with(kFontconfigPrefix)) {
        parsed = parse_fontconfig(identity, output, stats, error);
    } else if (identity.starts_with(kDirectWritePrefix)) {
        parsed = parse_directwrite(identity, output, stats, error);
    } else if (identity.starts_with(kCoreTextPrefix)) {
        parsed = parse_coretext(identity, output, stats, error);
    } else {
        return fail(
            FontLoadLocatorErrorKind::UnsupportedPrefix,
            0U,
            0U,
            "platform identity prefix is not supported",
            error);
    }

    if (!parsed) {
        *output = {};
        return false;
    }
    return true;
}

} // namespace zevryon::text
