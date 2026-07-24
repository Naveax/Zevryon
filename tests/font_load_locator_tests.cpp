#include "font_load_locator.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace zevryon::text;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

void append_field(std::string* identity, std::string_view value) {
    identity->append(std::to_string(value.size()));
    identity->push_back(':');
    identity->append(value);
    identity->push_back('|');
}

std::string fontconfig_identity(
    std::string_view sysroot,
    std::string_view file,
    std::string_view face,
    std::string_view postscript,
    std::string_view variations) {
    std::string result = "fontconfig|";
    append_field(&result, sysroot);
    append_field(&result, file);
    append_field(&result, face);
    append_field(&result, postscript);
    append_field(&result, variations);
    return result;
}

std::string directwrite_identity(
    const std::vector<std::string_view>& paths,
    std::string_view face_type,
    std::string_view face_index,
    std::string_view weight,
    std::string_view stretch,
    std::string_view style,
    std::string_view postscript) {
    std::string result = "directwrite|";
    append_field(&result, std::to_string(paths.size()));
    for (const std::string_view path : paths) {
        append_field(&result, path);
    }
    append_field(&result, face_type);
    append_field(&result, face_index);
    append_field(&result, weight);
    append_field(&result, stretch);
    append_field(&result, style);
    append_field(&result, postscript);
    return result;
}

std::string coretext_identity(
    std::string_view path,
    std::string_view postscript,
    std::string_view weight,
    std::string_view width,
    std::string_view slant,
    const std::vector<std::pair<std::string_view, std::string_view>>& variations) {
    std::string result = "coretext|";
    append_field(&result, path);
    append_field(&result, postscript);
    append_field(&result, weight);
    append_field(&result, width);
    append_field(&result, slant);
    append_field(&result, std::to_string(variations.size()));
    for (const auto& variation : variations) {
        append_field(&result, variation.first);
        append_field(&result, variation.second);
    }
    return result;
}

bool valid_fontconfig() {
    const std::string identity = fontconfig_identity(
        "/sys|root:one",
        "/fonts/A:B|C.ttc",
        "7",
        "Post|Script:Name",
        "wght=400|wdth:90");
    FontLoadLocator locator;
    FontLoadLocatorStats stats;
    FontLoadLocatorError error;
    const bool parsed =
        parse_font_load_locator(identity, &locator, &stats, &error);
    bool ok = expect(parsed, "valid Fontconfig identity must parse");
    ok &= expect(locator.kind == FontPlatformIdentityKind::Fontconfig &&
                     locator.capability ==
                         FontLoadCapability::SingleFileWithFaceIndex &&
                     locator.directly_loadable(),
                 "Fontconfig identity must be directly loadable");
    ok &= expect(locator.sysroot == "/sys|root:one" &&
                     locator.file_path == "/fonts/A:B|C.ttc" &&
                     locator.face_index == 7U && locator.has_face_index,
                 "Fontconfig load fields must be preserved exactly");
    ok &= expect(locator.postscript_name == "Post|Script:Name" &&
                     locator.variation_descriptor == "wght=400|wdth:90",
                 "Fontconfig diagnostic fields must preserve delimiters");
    ok &= expect(stats.identity_bytes == identity.size() &&
                     stats.fields_parsed == 5U && stats.file_count == 1U &&
                     stats.variation_count == 0U,
                 "Fontconfig parse stats must be exact");
    ok &= expect(locator.file_path.data() >= identity.data() &&
                     locator.file_path.data() < identity.data() + identity.size(),
                 "locator views must refer to caller-owned identity bytes");
    return ok;
}

bool valid_directwrite() {
    const std::string single = directwrite_identity(
        {"C:\\Fonts\\A|B:One.ttc"},
        "2",
        "11",
        "700",
        "5",
        "1",
        "DW:Post|Script");
    FontLoadLocator locator;
    FontLoadLocatorStats stats;
    FontLoadLocatorError error;
    bool ok = expect(
        parse_font_load_locator(single, &locator, &stats, &error),
        "single-file DirectWrite identity must parse");
    ok &= expect(locator.kind == FontPlatformIdentityKind::DirectWrite &&
                     locator.capability ==
                         FontLoadCapability::SingleFileWithFaceIndex &&
                     locator.directly_loadable(),
                 "single-file DirectWrite identity must be directly loadable");
    ok &= expect(locator.file_path == "C:\\Fonts\\A|B:One.ttc" &&
                     locator.face_index == 11U && locator.file_count == 1U &&
                     locator.postscript_name == "DW:Post|Script",
                 "DirectWrite single-file fields must be preserved");
    ok &= expect(stats.fields_parsed == 8U && stats.file_count == 1U,
                 "single-file DirectWrite field count must be exact");

    const std::string multiple = directwrite_identity(
        {"C:\\Fonts\\part1.ttf", "C:\\Fonts\\part2.ttf"},
        "2",
        "3",
        "400",
        "5",
        "0",
        "MultiFilePS");
    ok &= expect(parse_font_load_locator(multiple, &locator, &stats, &error),
                 "multi-file DirectWrite identity must be recognized");
    ok &= expect(locator.kind == FontPlatformIdentityKind::DirectWrite &&
                     locator.capability == FontLoadCapability::MultiFile &&
                     !locator.directly_loadable() && locator.file_path.empty() &&
                     locator.file_count == 2U && locator.face_index == 3U &&
                     locator.has_face_index,
                 "multi-file DirectWrite must not expose a portable single path");
    ok &= expect(stats.fields_parsed == 9U && stats.file_count == 2U,
                 "multi-file DirectWrite stats must be exact");
    return ok;
}

bool valid_coretext() {
    const std::string identity = coretext_identity(
        "/System/Library/Fonts/A:B|C.ttc",
        "Core|Text:PS",
        "650",
        "6",
        "1",
        {{"-123", "4607182418800017408"},
         {"456", "18446744073709551615"}});
    FontLoadLocator locator;
    FontLoadLocatorStats stats;
    FontLoadLocatorError error;
    const bool parsed =
        parse_font_load_locator(identity, &locator, &stats, &error);
    bool ok = expect(parsed, "valid CoreText identity must parse");
    ok &= expect(locator.kind == FontPlatformIdentityKind::CoreText &&
                     locator.capability ==
                         FontLoadCapability::SingleFileFaceIndexUnresolved &&
                     !locator.directly_loadable(),
                 "CoreText identity must remain face-index unresolved");
    ok &= expect(locator.file_path == "/System/Library/Fonts/A:B|C.ttc" &&
                     locator.postscript_name == "Core|Text:PS" &&
                     locator.file_count == 1U && !locator.has_face_index,
                 "CoreText path and PostScript name must be preserved");
    ok &= expect(stats.fields_parsed == 10U && stats.file_count == 1U &&
                     stats.variation_count == 2U,
                 "CoreText variation fields must be fully validated");
    return ok;
}

bool prefix_and_truncation_failures() {
    const std::string valid = fontconfig_identity(
        "", "/fonts/example.ttf", "0", "Example", "");
    bool ok = true;
    for (std::size_t size = 0U; size < valid.size(); ++size) {
        FontLoadLocator locator;
        locator.file_path = "stale";
        locator.file_count = 99U;
        FontLoadLocatorStats stats;
        stats.fields_parsed = 99U;
        FontLoadLocatorError error;
        const bool parsed = parse_font_load_locator(
            std::string_view(valid).substr(0U, size),
            &locator,
            &stats,
            &error);
        ok &= expect(!parsed, "every proper identity prefix must fail");
        ok &= expect(locator.file_path.empty() && locator.file_count == 0U,
                     "failure must reset locator output");
        ok &= expect(stats.identity_bytes == size,
                     "failure stats must preserve the examined byte count");
    }

    FontLoadLocator locator;
    FontLoadLocatorStats stats;
    FontLoadLocatorError error;
    ok &= expect(!parse_font_load_locator(
                     "unknown|1:a|", &locator, &stats, &error) &&
                     error.kind == FontLoadLocatorErrorKind::UnsupportedPrefix,
                 "unknown platform prefix must fail exactly");
    ok &= expect(!parse_font_load_locator({}, &locator, &stats, &error) &&
                     error.kind == FontLoadLocatorErrorKind::InvalidArgument,
                 "empty identity must fail as invalid argument");
    ok &= expect(!parse_font_load_locator(valid, nullptr, &stats, &error) &&
                     error.kind == FontLoadLocatorErrorKind::InvalidArgument,
                 "null locator output must fail");
    return ok;
}

bool malformed_field_encoding() {
    FontLoadLocator locator;
    FontLoadLocatorStats stats;
    FontLoadLocatorError error;
    bool ok = true;

    ok &= expect(!parse_font_load_locator(
                     "fontconfig|:|", &locator, &stats, &error) &&
                     error.kind == FontLoadLocatorErrorKind::MissingLength,
                 "empty length prefix must fail");
    ok &= expect(!parse_font_load_locator(
                     "fontconfig|1x:a|", &locator, &stats, &error) &&
                     error.kind ==
                         FontLoadLocatorErrorKind::NonCanonicalDecimal,
                 "non-decimal length must fail");
    ok &= expect(!parse_font_load_locator(
                     "fontconfig|01:a|", &locator, &stats, &error) &&
                     error.kind ==
                         FontLoadLocatorErrorKind::NonCanonicalDecimal,
                 "leading-zero length must fail");
    ok &= expect(!parse_font_load_locator(
                     "fontconfig|1a|", &locator, &stats, &error) &&
                     error.kind ==
                         FontLoadLocatorErrorKind::NonCanonicalDecimal,
                 "length without colon containing text must fail");
    ok &= expect(!parse_font_load_locator(
                     "fontconfig|2:a|", &locator, &stats, &error) &&
                     error.kind == FontLoadLocatorErrorKind::TruncatedField,
                 "payload shorter than length must fail");
    ok &= expect(!parse_font_load_locator(
                     "fontconfig|1:ax", &locator, &stats, &error) &&
                     error.kind ==
                         FontLoadLocatorErrorKind::MissingFieldTerminator,
                 "field without pipe terminator must fail");

    std::string embedded = "fontconfig|";
    append_field(&embedded, std::string_view("a\0b", 3U));
    ok &= expect(!parse_font_load_locator(
                     embedded, &locator, &stats, &error) &&
                     error.kind == FontLoadLocatorErrorKind::EmbeddedNull,
                 "embedded null field must fail");

    const std::string overflow =
        "fontconfig|999999999999999999999999999999999999999999:x|";
    ok &= expect(!parse_font_load_locator(
                     overflow, &locator, &stats, &error) &&
                     error.kind == FontLoadLocatorErrorKind::DecimalOverflow,
                 "field-length overflow must fail");
    return ok;
}

bool malformed_platform_fields() {
    FontLoadLocator locator;
    FontLoadLocatorStats stats;
    FontLoadLocatorError error;
    bool ok = true;

    ok &= expect(!parse_font_load_locator(
                     fontconfig_identity("", "", "0", "PS", ""),
                     &locator,
                     &stats,
                     &error) &&
                     error.kind ==
                         FontLoadLocatorErrorKind::EmptyRequiredField,
                 "Fontconfig empty file path must fail");
    ok &= expect(!parse_font_load_locator(
                     fontconfig_identity(
                         "", "/a.ttf", "01", "PS", ""),
                     &locator,
                     &stats,
                     &error) &&
                     error.kind ==
                         FontLoadLocatorErrorKind::NonCanonicalDecimal,
                 "Fontconfig noncanonical face index must fail");
    ok &= expect(!parse_font_load_locator(
                     fontconfig_identity(
                         "", "/a.ttf", "4294967296", "PS", ""),
                     &locator,
                     &stats,
                     &error) &&
                     error.kind == FontLoadLocatorErrorKind::DecimalOverflow,
                 "Fontconfig face index overflow must fail");

    ok &= expect(!parse_font_load_locator(
                     directwrite_identity(
                         {}, "1", "0", "400", "5", "0", "PS"),
                     &locator,
                     &stats,
                     &error) &&
                     error.kind == FontLoadLocatorErrorKind::InvalidFieldCount,
                 "DirectWrite zero file count must fail");
    std::string huge_directwrite = "directwrite|";
    append_field(&huge_directwrite, "4090");
    ok &= expect(!parse_font_load_locator(
                     huge_directwrite, &locator, &stats, &error) &&
                     error.kind == FontLoadLocatorErrorKind::TooManyFields,
                 "DirectWrite excessive file count must fail before looping");

    ok &= expect(!parse_font_load_locator(
                     coretext_identity(
                         "/a.ttc", "PS", "0", "5", "0", {}),
                     &locator,
                     &stats,
                     &error) &&
                     error.kind ==
                         FontLoadLocatorErrorKind::InvalidNumericValue,
                 "CoreText weight below range must fail");
    ok &= expect(!parse_font_load_locator(
                     coretext_identity(
                         "/a.ttc", "PS", "400", "10", "0", {}),
                     &locator,
                     &stats,
                     &error) &&
                     error.kind ==
                         FontLoadLocatorErrorKind::InvalidNumericValue,
                 "CoreText width above range must fail");
    ok &= expect(!parse_font_load_locator(
                     coretext_identity(
                         "/a.ttc", "PS", "400", "5", "3", {}),
                     &locator,
                     &stats,
                     &error) &&
                     error.kind ==
                         FontLoadLocatorErrorKind::InvalidNumericValue,
                 "CoreText slant above range must fail");

    std::string missing_variation = "coretext|";
    append_field(&missing_variation, "/a.ttc");
    append_field(&missing_variation, "PS");
    append_field(&missing_variation, "400");
    append_field(&missing_variation, "5");
    append_field(&missing_variation, "0");
    append_field(&missing_variation, "1");
    append_field(&missing_variation, "123");
    ok &= expect(!parse_font_load_locator(
                     missing_variation, &locator, &stats, &error) &&
                     error.kind == FontLoadLocatorErrorKind::TruncatedField,
                 "CoreText incomplete variation pair must fail");

    ok &= expect(!parse_font_load_locator(
                     coretext_identity(
                         "/a.ttc", "PS", "400", "5", "0", {{"-0", "1"}}),
                     &locator,
                     &stats,
                     &error) &&
                     error.kind ==
                         FontLoadLocatorErrorKind::NonCanonicalDecimal,
                 "CoreText negative-zero variation identifier must fail");
    ok &= expect(!parse_font_load_locator(
                     coretext_identity(
                         "/a.ttc", "PS", "400", "5", "0",
                         {{"9223372036854775808", "1"}}),
                     &locator,
                     &stats,
                     &error) &&
                     error.kind == FontLoadLocatorErrorKind::DecimalOverflow,
                 "CoreText signed variation identifier overflow must fail");

    std::string trailing = fontconfig_identity(
        "", "/a.ttf", "0", "PS", "");
    trailing.push_back('x');
    ok &= expect(!parse_font_load_locator(
                     trailing, &locator, &stats, &error) &&
                     error.kind == FontLoadLocatorErrorKind::TrailingData,
                 "trailing identity data must fail");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= valid_fontconfig();
    ok &= valid_directwrite();
    ok &= valid_coretext();
    ok &= prefix_and_truncation_failures();
    ok &= malformed_field_encoding();
    ok &= malformed_platform_fields();
    if (!ok) {
        return 1;
    }
    std::cout << "font-load-locator-tests: PASS\n";
    return 0;
}
