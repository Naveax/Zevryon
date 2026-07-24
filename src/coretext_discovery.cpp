#include "coretext_discovery.hpp"

#include "unicode_script.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zevryon::text {
namespace {

constexpr std::size_t kScriptCount = static_cast<std::size_t>(ScriptId::Count);
constexpr std::uint32_t kMaximumCodepoint = 0x10ffffU;
constexpr CFIndex kUnicodePlaneCount = 17;

template <typename T>
class CfOwner final {
public:
    explicit CfOwner(T value = nullptr) noexcept : value_(value) {}
    ~CfOwner() {
        if (value_ != nullptr) {
            CFRelease(value_);
        }
    }

    CfOwner(const CfOwner&) = delete;
    CfOwner& operator=(const CfOwner&) = delete;

    CfOwner(CfOwner&& other) noexcept : value_(other.release()) {}
    CfOwner& operator=(CfOwner&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    T get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

    T release() noexcept {
        T value = value_;
        value_ = nullptr;
        return value;
    }

    void reset(T value = nullptr) noexcept {
        if (value_ != nullptr) {
            CFRelease(value_);
        }
        value_ = value;
    }

private:
    T value_{nullptr};
};

struct VariationCoordinate {
    std::int64_t axis{0};
    std::uint64_t value_bits{0};

    bool operator<(const VariationCoordinate& other) const noexcept {
        return axis < other.axis ||
            (axis == other.axis && value_bits < other.value_bits);
    }
};

struct OwnedFace {
    std::string identity;
    std::string family;
    std::uint16_t weight{400};
    std::uint8_t width{5};
    FontSlant slant{FontSlant::Upright};
    ScriptId preferred_script{ScriptId::Zyyy};
    std::uint16_t flags{0};
    std::vector<FontCoverageRange> coverage;
};

void clear_error(CoreTextDiscoveryError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    CoreTextDiscoveryErrorKind kind,
    std::size_t descriptor_index,
    const char* message,
    CoreTextDiscoveryError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->descriptor_index = descriptor_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool cf_string_utf8(CFStringRef value, std::string* output) {
    if (value == nullptr || output == nullptr ||
        CFGetTypeID(value) != CFStringGetTypeID()) {
        return false;
    }
    const CFIndex length = CFStringGetLength(value);
    if (length <= 0) {
        return false;
    }
    const CFIndex maximum = CFStringGetMaximumSizeForEncoding(
        length,
        kCFStringEncodingUTF8);
    if (maximum <= 0 || maximum >= std::numeric_limits<CFIndex>::max()) {
        return false;
    }
    std::vector<char> buffer(static_cast<std::size_t>(maximum) + 1U, '\0');
    if (!CFStringGetCString(
            value,
            buffer.data(),
            static_cast<CFIndex>(buffer.size()),
            kCFStringEncodingUTF8)) {
        return false;
    }
    output->assign(buffer.data());
    return !output->empty() && output->find('\0') == std::string::npos;
}

bool descriptor_string(
    CTFontDescriptorRef descriptor,
    CFStringRef key,
    std::string* output) {
    CfOwner<CFTypeRef> value(CTFontDescriptorCopyAttribute(descriptor, key));
    return value && CFGetTypeID(value.get()) == CFStringGetTypeID() &&
        cf_string_utf8(static_cast<CFStringRef>(value.get()), output);
}

bool descriptor_file_path(CTFontDescriptorRef descriptor, std::string* output) {
    CfOwner<CFTypeRef> value(
        CTFontDescriptorCopyAttribute(descriptor, kCTFontURLAttribute));
    if (!value || CFGetTypeID(value.get()) != CFURLGetTypeID()) {
        return false;
    }
    CFURLRef url = static_cast<CFURLRef>(value.get());
    if (!CFURLIsFileURL(url)) {
        return false;
    }
    CfOwner<CFStringRef> path(
        CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle));
    return path && cf_string_utf8(path.get(), output);
}

bool number_double(CFTypeRef value, double* output) noexcept {
    if (value == nullptr || output == nullptr ||
        CFGetTypeID(value) != CFNumberGetTypeID()) {
        return false;
    }
    double converted = 0.0;
    if (!CFNumberGetValue(
            static_cast<CFNumberRef>(value),
            kCFNumberDoubleType,
            &converted) ||
        !std::isfinite(converted)) {
        return false;
    }
    *output = converted;
    return true;
}

bool number_i64(CFTypeRef value, std::int64_t* output) noexcept {
    if (value == nullptr || output == nullptr ||
        CFGetTypeID(value) != CFNumberGetTypeID()) {
        return false;
    }
    std::int64_t converted = 0;
    if (!CFNumberGetValue(
            static_cast<CFNumberRef>(value),
            kCFNumberSInt64Type,
            &converted)) {
        return false;
    }
    *output = converted;
    return true;
}

bool dictionary_number(
    CFDictionaryRef dictionary,
    CFStringRef key,
    double default_value,
    double* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    *output = default_value;
    if (dictionary == nullptr) {
        return true;
    }
    const void* value = CFDictionaryGetValue(dictionary, key);
    return value == nullptr || number_double(value, output);
}

std::uint16_t css_weight(double normalized) noexcept {
    normalized = std::clamp(normalized, -1.0, 1.0);
    const double value = normalized < 0.0
        ? 400.0 + normalized * 399.0
        : 400.0 + normalized * 600.0;
    return static_cast<std::uint16_t>(
        std::clamp(static_cast<int>(std::lround(value)), 1, 1000));
}

std::uint8_t css_width(double normalized) noexcept {
    normalized = std::clamp(normalized, -1.0, 1.0);
    const int value = static_cast<int>(std::lround(5.0 + normalized * 4.0));
    return static_cast<std::uint8_t>(std::clamp(value, 1, 9));
}

FontSlant font_slant(
    CTFontSymbolicTraits symbolic,
    double normalized_slant) noexcept {
    if ((symbolic & kCTFontTraitItalic) != 0U) {
        return FontSlant::Italic;
    }
    return std::abs(normalized_slant) > 0.000001
        ? FontSlant::Oblique
        : FontSlant::Upright;
}

void append_identity_field(std::string* output, std::string_view value) {
    output->append(std::to_string(value.size()));
    output->push_back(':');
    output->append(value);
    output->push_back('|');
}

void append_identity_integer(std::string* output, std::uint64_t value) {
    append_identity_field(output, std::to_string(value));
}

bool variation_coordinates(
    CTFontRef font,
    std::vector<VariationCoordinate>* output) {
    if (font == nullptr || output == nullptr) {
        return false;
    }
    output->clear();
    CfOwner<CFDictionaryRef> variation(CTFontCopyVariation(font));
    if (!variation || CFDictionaryGetCount(variation.get()) == 0) {
        return true;
    }
    const CFIndex count = CFDictionaryGetCount(variation.get());
    if (count < 0) {
        return false;
    }
    std::vector<const void*> keys(static_cast<std::size_t>(count));
    std::vector<const void*> values(static_cast<std::size_t>(count));
    CFDictionaryGetKeysAndValues(
        variation.get(),
        keys.data(),
        values.data());
    output->reserve(static_cast<std::size_t>(count));
    for (CFIndex index = 0; index < count; ++index) {
        std::int64_t axis = 0;
        double value = 0.0;
        const std::size_t offset = static_cast<std::size_t>(index);
        if (!number_i64(keys[offset], &axis) ||
            !number_double(values[offset], &value)) {
            return false;
        }
        output->push_back(VariationCoordinate{
            axis,
            std::bit_cast<std::uint64_t>(value)});
    }
    std::sort(output->begin(), output->end());
    return std::adjacent_find(
               output->begin(),
               output->end(),
               [](const VariationCoordinate& left, const VariationCoordinate& right) {
                   return left.axis == right.axis;
               }) == output->end();
}

std::string make_identity(
    std::string_view path,
    std::string_view postscript,
    std::string_view family,
    CTFontSymbolicTraits symbolic,
    std::uint16_t weight,
    std::uint8_t width,
    FontSlant slant,
    const std::vector<VariationCoordinate>& variations) {
    std::string identity;
    identity.reserve(path.size() + postscript.size() + family.size() + 128U);
    identity.append("coretext|");
    append_identity_field(&identity, path);
    append_identity_field(&identity, postscript);
    append_identity_field(&identity, family);
    append_identity_integer(&identity, static_cast<std::uint64_t>(symbolic));
    append_identity_integer(&identity, weight);
    append_identity_integer(&identity, width);
    append_identity_integer(&identity, static_cast<std::uint64_t>(slant));
    append_identity_integer(&identity, variations.size());
    for (const VariationCoordinate& coordinate : variations) {
        append_identity_integer(
            &identity,
            static_cast<std::uint64_t>(coordinate.axis));
        append_identity_integer(&identity, coordinate.value_bits);
    }
    return identity;
}

bool canonical_coverage(
    CTFontRef font,
    std::vector<FontCoverageRange>* ranges,
    ScriptId* preferred_script,
    std::uint64_t* codepoint_count) {
    if (font == nullptr || ranges == nullptr || preferred_script == nullptr ||
        codepoint_count == nullptr) {
        return false;
    }
    CfOwner<CFCharacterSetRef> characters(CTFontCopyCharacterSet(font));
    if (!characters) {
        return false;
    }

    ranges->clear();
    *codepoint_count = 0U;
    std::array<std::uint64_t, kScriptCount> script_counts{};
    bool in_range = false;
    std::uint32_t range_start = 0U;

    for (CFIndex plane = 0; plane < kUnicodePlaneCount; ++plane) {
        const std::uint32_t plane_start =
            static_cast<std::uint32_t>(plane) << 16U;
        const std::uint32_t plane_last = std::min(
            plane_start + 0xffffU,
            kMaximumCodepoint);
        if (!CFCharacterSetHasMemberInPlane(characters.get(), plane)) {
            if (in_range) {
                ranges->push_back(FontCoverageRange{
                    range_start,
                    plane_start - 1U});
                in_range = false;
            }
            continue;
        }

        for (std::uint32_t codepoint = plane_start;; ++codepoint) {
            const bool member =
                CFCharacterSetIsLongCharacterMember(characters.get(), codepoint);
            if (member && !in_range) {
                range_start = codepoint;
                in_range = true;
            } else if (!member && in_range) {
                ranges->push_back(FontCoverageRange{range_start, codepoint - 1U});
                in_range = false;
            }
            if (member) {
                ++*codepoint_count;
                const ScriptId script = script_of(codepoint);
                if (!is_neutral_script(script)) {
                    const std::size_t script_index = static_cast<std::size_t>(script);
                    if (script_index < script_counts.size()) {
                        ++script_counts[script_index];
                    }
                }
            }
            if (codepoint == plane_last) {
                break;
            }
        }
    }
    if (in_range) {
        ranges->push_back(FontCoverageRange{range_start, kMaximumCodepoint});
    }
    if (ranges->empty()) {
        return false;
    }

    const auto best = std::max_element(script_counts.begin(), script_counts.end());
    *preferred_script = best != script_counts.end() && *best != 0U
        ? static_cast<ScriptId>(
              static_cast<std::size_t>(best - script_counts.begin()))
        : ScriptId::Zyyy;
    return true;
}

bool same_face(const OwnedFace& left, const OwnedFace& right) noexcept {
    return left.identity == right.identity && left.family == right.family &&
           left.weight == right.weight && left.width == right.width &&
           left.slant == right.slant &&
           left.preferred_script == right.preferred_script &&
           left.flags == right.flags && left.coverage == right.coverage;
}

} // namespace

const char* coretext_discovery_error_kind_name(
    CoreTextDiscoveryErrorKind kind) noexcept {
    switch (kind) {
        case CoreTextDiscoveryErrorKind::None:
            return "none";
        case CoreTextDiscoveryErrorKind::CollectionCreationFailed:
            return "collection_creation_failed";
        case CoreTextDiscoveryErrorKind::EnumerationFailed:
            return "enumeration_failed";
        case CoreTextDiscoveryErrorKind::MissingRequiredProperty:
            return "missing_required_property";
        case CoreTextDiscoveryErrorKind::InvalidNativeValue:
            return "invalid_native_value";
        case CoreTextDiscoveryErrorKind::ConflictingDuplicate:
            return "conflicting_duplicate";
        case CoreTextDiscoveryErrorKind::SnapshotBuildFailed:
            return "snapshot_build_failed";
    }
    return "invalid";
}

bool build_coretext_generation(
    std::uint64_t generation_id,
    std::size_t discovery_hard_limit,
    std::size_t catalog_hard_limit,
    std::shared_ptr<const FontCatalogGeneration>* output,
    CoreTextDiscoveryStats* stats,
    CoreTextDiscoveryError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->reset();
    *stats = {};
    clear_error(error);

    try {
        CfOwner<CTFontCollectionRef> collection(
            CTFontCollectionCreateFromAvailableFonts(nullptr));
        if (!collection) {
            return fail(
                CoreTextDiscoveryErrorKind::CollectionCreationFailed,
                0U,
                "CoreText available-font collection creation failed",
                error);
        }
        CfOwner<CFArrayRef> descriptors(
            CTFontCollectionCreateMatchingFontDescriptors(collection.get()));
        if (!descriptors) {
            return fail(
                CoreTextDiscoveryErrorKind::EnumerationFailed,
                0U,
                "CoreText descriptor enumeration failed",
                error);
        }
        const CFIndex descriptor_count = CFArrayGetCount(descriptors.get());
        if (descriptor_count <= 0) {
            return fail(
                CoreTextDiscoveryErrorKind::EnumerationFailed,
                0U,
                "CoreText available-font collection is empty",
                error);
        }
        stats->descriptors_seen = static_cast<std::uint64_t>(descriptor_count);

        std::vector<OwnedFace> owned;
        owned.reserve(static_cast<std::size_t>(descriptor_count));
        for (CFIndex descriptor_index = 0;
             descriptor_index < descriptor_count;
             ++descriptor_index) {
            const auto* descriptor = static_cast<CTFontDescriptorRef>(
                CFArrayGetValueAtIndex(descriptors.get(), descriptor_index));
            if (descriptor == nullptr ||
                CFGetTypeID(descriptor) != CTFontDescriptorGetTypeID()) {
                return fail(
                    CoreTextDiscoveryErrorKind::InvalidNativeValue,
                    static_cast<std::size_t>(descriptor_index),
                    "CoreText collection returned an invalid descriptor",
                    error);
            }

            std::string path;
            if (!descriptor_file_path(descriptor, &path)) {
                ++stats->non_file_descriptors_skipped;
                continue;
            }
            CfOwner<CTFontRef> font(
                CTFontCreateWithFontDescriptor(descriptor, 0.0, nullptr));
            if (!font) {
                return fail(
                    CoreTextDiscoveryErrorKind::EnumerationFailed,
                    static_cast<std::size_t>(descriptor_index),
                    "CoreText failed to create a font from a normalized descriptor",
                    error);
            }

            OwnedFace face;
            CfOwner<CFStringRef> family(CTFontCopyFamilyName(font.get()));
            CfOwner<CFStringRef> postscript(CTFontCopyPostScriptName(font.get()));
            std::string postscript_name;
            if (!family || !postscript ||
                !cf_string_utf8(family.get(), &face.family) ||
                !cf_string_utf8(postscript.get(), &postscript_name)) {
                return fail(
                    CoreTextDiscoveryErrorKind::MissingRequiredProperty,
                    static_cast<std::size_t>(descriptor_index),
                    "CoreText font lacks a valid family or PostScript name",
                    error);
            }

            CfOwner<CFDictionaryRef> traits(CTFontCopyTraits(font.get()));
            double normalized_weight = 0.0;
            double normalized_width = 0.0;
            double normalized_slant = 0.0;
            if (!dictionary_number(
                    traits.get(),
                    kCTFontWeightTrait,
                    0.0,
                    &normalized_weight) ||
                !dictionary_number(
                    traits.get(),
                    kCTFontWidthTrait,
                    0.0,
                    &normalized_width) ||
                !dictionary_number(
                    traits.get(),
                    kCTFontSlantTrait,
                    0.0,
                    &normalized_slant)) {
                return fail(
                    CoreTextDiscoveryErrorKind::InvalidNativeValue,
                    static_cast<std::size_t>(descriptor_index),
                    "CoreText returned invalid normalized trait data",
                    error);
            }
            const CTFontSymbolicTraits symbolic =
                CTFontGetSymbolicTraits(font.get());
            face.weight = css_weight(normalized_weight);
            face.width = css_width(normalized_width);
            face.slant = font_slant(symbolic, normalized_slant);
            face.flags = kFontFaceSystem;
            if ((symbolic & kCTFontTraitMonoSpace) != 0U) {
                face.flags = static_cast<std::uint16_t>(
                    face.flags | kFontFaceMonospace);
                ++stats->monospace_faces;
            }
            if ((symbolic & kCTFontTraitColorGlyphs) != 0U) {
                face.flags = static_cast<std::uint16_t>(
                    face.flags | kFontFaceColor);
                ++stats->color_faces;
            }

            CfOwner<CFArrayRef> variation_axes(
                CTFontCopyVariationAxes(font.get()));
            if (variation_axes && CFArrayGetCount(variation_axes.get()) > 0) {
                face.flags = static_cast<std::uint16_t>(
                    face.flags | kFontFaceVariable);
                ++stats->variable_faces;
            }
            std::vector<VariationCoordinate> variations;
            if (!variation_coordinates(font.get(), &variations)) {
                return fail(
                    CoreTextDiscoveryErrorKind::InvalidNativeValue,
                    static_cast<std::size_t>(descriptor_index),
                    "CoreText returned invalid variation coordinates",
                    error);
            }

            std::uint64_t coverage_codepoints = 0U;
            if (!canonical_coverage(
                    font.get(),
                    &face.coverage,
                    &face.preferred_script,
                    &coverage_codepoints)) {
                return fail(
                    CoreTextDiscoveryErrorKind::InvalidNativeValue,
                    static_cast<std::size_t>(descriptor_index),
                    "CoreText returned empty or invalid Unicode coverage",
                    error);
            }
            stats->coverage_codepoints += coverage_codepoints;
            stats->coverage_ranges +=
                static_cast<std::uint64_t>(face.coverage.size());
            face.identity = make_identity(
                path,
                postscript_name,
                face.family,
                symbolic,
                face.weight,
                face.width,
                face.slant,
                variations);
            owned.push_back(std::move(face));
        }

        std::sort(
            owned.begin(),
            owned.end(),
            [](const OwnedFace& left, const OwnedFace& right) {
                return left.identity < right.identity;
            });
        std::vector<OwnedFace> unique;
        unique.reserve(owned.size());
        for (OwnedFace& face : owned) {
            if (!unique.empty() && unique.back().identity == face.identity) {
                if (!same_face(unique.back(), face)) {
                    return fail(
                        CoreTextDiscoveryErrorKind::ConflictingDuplicate,
                        unique.size(),
                        "CoreText returned conflicting metadata for one identity",
                        error);
                }
                ++stats->duplicate_faces_skipped;
                continue;
            }
            unique.push_back(std::move(face));
        }
        if (unique.empty()) {
            return fail(
                CoreTextDiscoveryErrorKind::EnumerationFailed,
                0U,
                "CoreText produced no physical file-backed faces",
                error);
        }

        std::vector<FontDiscoveryFace> views;
        views.reserve(unique.size());
        for (const OwnedFace& face : unique) {
            views.push_back(FontDiscoveryFace{
                face.identity,
                face.family,
                face.weight,
                face.width,
                face.slant,
                face.preferred_script,
                face.flags,
                face.coverage});
        }

        FontDiscoveryError generation_error;
        if (!build_font_catalog_generation(
                generation_id,
                views,
                discovery_hard_limit,
                catalog_hard_limit,
                output,
                &stats->generation,
                &generation_error)) {
            error->kind = CoreTextDiscoveryErrorKind::SnapshotBuildFailed;
            error->generation_error = generation_error.kind;
            error->catalog_error = generation_error.catalog_error;
            error->message = generation_error.message;
            return false;
        }
        stats->faces_emitted = static_cast<std::uint64_t>(unique.size());
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            CoreTextDiscoveryErrorKind::EnumerationFailed,
            0U,
            "CoreText adapter allocation failed",
            error);
    } catch (...) {
        return fail(
            CoreTextDiscoveryErrorKind::EnumerationFailed,
            0U,
            "CoreText adapter failed",
            error);
    }
}

} // namespace zevryon::text
