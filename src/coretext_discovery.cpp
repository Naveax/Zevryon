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

constexpr std::size_t kPlaneBytes = 8192U;
constexpr std::size_t kUnicodePlaneCount = 17U;
constexpr std::size_t kScriptCount = static_cast<std::size_t>(ScriptId::Count);

template <typename T>
class CfOwned final {
public:
    explicit CfOwned(T value = nullptr) noexcept : value_(value) {}

    ~CfOwned() {
        if (value_ != nullptr) {
            CFRelease(value_);
        }
    }

    CfOwned(const CfOwned&) = delete;
    CfOwned& operator=(const CfOwned&) = delete;

    CfOwned(CfOwned&& other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }

    CfOwned& operator=(CfOwned&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr) {
                CFRelease(value_);
            }
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }

    T get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    T value_{nullptr};
};

struct VariationCoordinate {
    std::int64_t identifier{0};
    std::uint64_t value_bits{0};

    bool operator==(const VariationCoordinate&) const noexcept = default;
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

bool cf_string_to_utf8(CFStringRef input, std::string* output) {
    if (input == nullptr || output == nullptr) {
        return false;
    }
    const CFIndex length = CFStringGetLength(input);
    if (length <= 0) {
        return false;
    }

    CFIndex required = 0;
    const CFIndex converted = CFStringGetBytes(
        input,
        CFRangeMake(0, length),
        kCFStringEncodingUTF8,
        0U,
        false,
        nullptr,
        0,
        &required);
    if (converted != length || required <= 0) {
        return false;
    }
    if (static_cast<std::uint64_t>(required) >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }

    std::vector<UInt8> bytes(static_cast<std::size_t>(required));
    CFIndex written = 0;
    const CFIndex second_converted = CFStringGetBytes(
        input,
        CFRangeMake(0, length),
        kCFStringEncodingUTF8,
        0U,
        false,
        bytes.data(),
        required,
        &written);
    if (second_converted != length || written != required) {
        return false;
    }

    output->assign(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::size_t>(written));
    return !output->empty() && output->find('\0') == std::string::npos;
}

bool descriptor_file_path(CTFontDescriptorRef descriptor, std::string* output) {
    if (descriptor == nullptr || output == nullptr) {
        return false;
    }
    CfOwned<CFTypeRef> attribute(
        CTFontDescriptorCopyAttribute(descriptor, kCTFontURLAttribute));
    if (!attribute || CFGetTypeID(attribute.get()) != CFURLGetTypeID()) {
        return false;
    }

    const auto url = static_cast<CFURLRef>(attribute.get());
    CfOwned<CFURLRef> absolute(CFURLCopyAbsoluteURL(url));
    if (!absolute) {
        return false;
    }
    CfOwned<CFStringRef> path(
        CFURLCopyFileSystemPath(absolute.get(), kCFURLPOSIXPathStyle));
    return path && cf_string_to_utf8(path.get(), output) &&
           !output->empty() && output->front() == '/';
}

bool font_names(
    CTFontRef font,
    std::string* postscript,
    std::string* family) {
    if (font == nullptr || postscript == nullptr || family == nullptr) {
        return false;
    }
    CfOwned<CFStringRef> postscript_name(CTFontCopyPostScriptName(font));
    CfOwned<CFStringRef> family_name(CTFontCopyFamilyName(font));
    return postscript_name && family_name &&
           cf_string_to_utf8(postscript_name.get(), postscript) &&
           cf_string_to_utf8(family_name.get(), family);
}

bool normalized_trait(
    CFDictionaryRef traits,
    CFStringRef key,
    double* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    *output = 0.0;
    if (traits == nullptr) {
        return true;
    }
    const void* raw = CFDictionaryGetValue(traits, key);
    if (raw == nullptr) {
        return true;
    }
    const auto value = static_cast<CFNumberRef>(raw);
    if (CFGetTypeID(value) != CFNumberGetTypeID()) {
        return false;
    }
    double number = 0.0;
    if (!CFNumberGetValue(value, kCFNumberDoubleType, &number) ||
        !std::isfinite(number)) {
        return false;
    }
    *output = std::clamp(number, -1.0, 1.0);
    return true;
}

std::uint16_t css_weight(
    double normalized,
    CTFontSymbolicTraits symbolic) noexcept {
    const double mapped = normalized >= 0.0
        ? 400.0 + normalized * 500.0
        : 400.0 + normalized * 300.0;
    long rounded = std::lround(mapped);
    if ((symbolic & kCTFontTraitBold) != 0U && rounded < 600L) {
        rounded = 700L;
    }
    rounded = std::clamp(rounded, 1L, 1000L);
    return static_cast<std::uint16_t>(rounded);
}

std::uint8_t css_width(double normalized) noexcept {
    const long rounded = std::clamp(
        std::lround(5.0 + normalized * 4.0),
        1L,
        9L);
    return static_cast<std::uint8_t>(rounded);
}

FontSlant font_slant(
    double normalized,
    CTFontSymbolicTraits symbolic) noexcept {
    if ((symbolic & kCTFontTraitItalic) != 0U) {
        return FontSlant::Italic;
    }
    return std::abs(normalized) > 0.000001
        ? FontSlant::Oblique
        : FontSlant::Upright;
}

bool variation_coordinates(
    CTFontRef font,
    std::vector<VariationCoordinate>* output) {
    if (font == nullptr || output == nullptr) {
        return false;
    }
    output->clear();
    CfOwned<CFDictionaryRef> variation(CTFontCopyVariation(font));
    if (!variation) {
        return true;
    }
    const CFIndex count = CFDictionaryGetCount(variation.get());
    if (count < 0 || static_cast<std::uint64_t>(count) >
                         static_cast<std::uint64_t>(
                             std::numeric_limits<std::size_t>::max())) {
        return false;
    }

    std::vector<const void*> keys(static_cast<std::size_t>(count));
    std::vector<const void*> values(static_cast<std::size_t>(count));
    if (count != 0) {
        CFDictionaryGetKeysAndValues(
            variation.get(),
            keys.data(),
            values.data());
    }
    output->reserve(static_cast<std::size_t>(count));
    for (CFIndex index = 0; index < count; ++index) {
        const auto key = static_cast<CFNumberRef>(
            keys[static_cast<std::size_t>(index)]);
        const auto value = static_cast<CFNumberRef>(
            values[static_cast<std::size_t>(index)]);
        if (key == nullptr || value == nullptr ||
            CFGetTypeID(key) != CFNumberGetTypeID() ||
            CFGetTypeID(value) != CFNumberGetTypeID()) {
            return false;
        }
        std::int64_t identifier = 0;
        double coordinate = 0.0;
        if (!CFNumberGetValue(key, kCFNumberSInt64Type, &identifier) ||
            !CFNumberGetValue(value, kCFNumberDoubleType, &coordinate) ||
            !std::isfinite(coordinate)) {
            return false;
        }
        output->push_back({
            identifier,
            std::bit_cast<std::uint64_t>(coordinate)});
    }
    std::sort(
        output->begin(),
        output->end(),
        [](const VariationCoordinate& left, const VariationCoordinate& right) {
            return left.identifier < right.identifier ||
                   (left.identifier == right.identifier &&
                    left.value_bits < right.value_bits);
        });
    for (std::size_t index = 1U; index < output->size(); ++index) {
        if ((*output)[index - 1U].identifier == (*output)[index].identifier) {
            return false;
        }
    }
    return true;
}

void append_identity_field(std::string* output, std::string_view value) {
    output->append(std::to_string(value.size()));
    output->push_back(':');
    output->append(value);
    output->push_back('|');
}

std::string make_identity(
    std::string_view path,
    std::string_view postscript,
    std::uint16_t weight,
    std::uint8_t width,
    FontSlant slant,
    const std::vector<VariationCoordinate>& variations) {
    std::string identity;
    identity.reserve(path.size() + postscript.size() + 128U +
                     variations.size() * 48U);
    identity.append("coretext|");
    append_identity_field(&identity, path);
    append_identity_field(&identity, postscript);
    append_identity_field(&identity, std::to_string(weight));
    append_identity_field(&identity, std::to_string(width));
    append_identity_field(
        &identity,
        std::to_string(static_cast<unsigned>(slant)));
    append_identity_field(&identity, std::to_string(variations.size()));
    for (const VariationCoordinate coordinate : variations) {
        append_identity_field(
            &identity,
            std::to_string(coordinate.identifier));
        append_identity_field(
            &identity,
            std::to_string(coordinate.value_bits));
    }
    return identity;
}

void append_coverage_range(
    std::vector<FontCoverageRange>* output,
    std::uint32_t first,
    std::uint32_t last) {
    if (!output->empty() &&
        static_cast<std::uint64_t>(first) <=
            static_cast<std::uint64_t>(output->back().last) + 1U) {
        output->back().last = std::max(output->back().last, last);
    } else {
        output->push_back({first, last});
    }
}

bool character_set_coverage(
    CFCharacterSetRef character_set,
    std::vector<FontCoverageRange>* output,
    ScriptId* preferred_script,
    std::uint64_t* codepoint_count,
    std::uint64_t* plane_count) {
    if (character_set == nullptr || output == nullptr ||
        preferred_script == nullptr || codepoint_count == nullptr ||
        plane_count == nullptr) {
        return false;
    }

    CfOwned<CFDataRef> data(
        CFCharacterSetCreateBitmapRepresentation(nullptr, character_set));
    if (!data) {
        return false;
    }
    const CFIndex length = CFDataGetLength(data.get());
    if (length < static_cast<CFIndex>(kPlaneBytes)) {
        return false;
    }
    const UInt8* bytes = CFDataGetBytePtr(data.get());
    if (bytes == nullptr) {
        return false;
    }

    std::array<const UInt8*, kUnicodePlaneCount> planes{};
    planes[0] = bytes;
    std::size_t position = kPlaneBytes;
    const std::size_t byte_count = static_cast<std::size_t>(length);
    while (position < byte_count) {
        if (byte_count - position < kPlaneBytes + 1U) {
            return false;
        }
        const std::uint8_t plane = bytes[position];
        ++position;
        if (plane == 0U || plane >= kUnicodePlaneCount ||
            planes[plane] != nullptr) {
            return false;
        }
        planes[plane] = bytes + position;
        position += kPlaneBytes;
    }

    output->clear();
    std::array<std::uint64_t, kScriptCount> script_counts{};
    std::uint64_t total = 0U;
    std::uint64_t populated_planes = 0U;

    for (std::size_t plane = 0U; plane < planes.size(); ++plane) {
        const UInt8* bitmap = planes[plane];
        if (bitmap == nullptr) {
            continue;
        }
        ++populated_planes;
        bool in_range = false;
        std::uint32_t range_first = 0U;
        for (std::uint32_t local = 0U; local <= 0xffffU; ++local) {
            const bool member =
                (bitmap[local >> 3U] &
                 static_cast<UInt8>(1U << (local & 7U))) != 0U;
            const std::uint32_t scalar =
                static_cast<std::uint32_t>((plane << 16U) | local);
            const bool valid_member = member &&
                !(scalar >= 0xd800U && scalar <= 0xdfffU);

            if (valid_member) {
                if (!in_range) {
                    range_first = scalar;
                    in_range = true;
                }
                ++total;
                const ScriptId script = script_of(scalar);
                if (!is_neutral_script(script)) {
                    const std::size_t script_index =
                        static_cast<std::size_t>(script);
                    if (script_index < script_counts.size()) {
                        ++script_counts[script_index];
                    }
                }
            } else if (in_range) {
                append_coverage_range(output, range_first, scalar - 1U);
                in_range = false;
            }
        }
        if (in_range) {
            const std::uint32_t last = static_cast<std::uint32_t>(
                (plane << 16U) | 0xffffU);
            append_coverage_range(output, range_first, last);
        }
    }

    if (output->empty() || total == 0U) {
        return false;
    }
    const auto best = std::max_element(script_counts.begin(), script_counts.end());
    *preferred_script = best != script_counts.end() && *best != 0U
        ? static_cast<ScriptId>(
              static_cast<std::size_t>(best - script_counts.begin()))
        : ScriptId::Zyyy;
    *codepoint_count = total;
    *plane_count = populated_planes;
    return true;
}

bool font_metadata(
    CTFontRef font,
    std::uint16_t* weight,
    std::uint8_t* width,
    FontSlant* slant,
    std::uint16_t* flags) {
    if (font == nullptr || weight == nullptr || width == nullptr ||
        slant == nullptr || flags == nullptr) {
        return false;
    }
    CfOwned<CFDictionaryRef> traits(CTFontCopyTraits(font));
    double normalized_weight = 0.0;
    double normalized_width = 0.0;
    double normalized_slant = 0.0;
    if (!normalized_trait(traits.get(), kCTFontWeightTrait, &normalized_weight) ||
        !normalized_trait(traits.get(), kCTFontWidthTrait, &normalized_width) ||
        !normalized_trait(traits.get(), kCTFontSlantTrait, &normalized_slant)) {
        return false;
    }

    const CTFontSymbolicTraits symbolic = CTFontGetSymbolicTraits(font);
    *weight = css_weight(normalized_weight, symbolic);
    *width = css_width(normalized_width);
    *slant = font_slant(normalized_slant, symbolic);
    *flags = kFontFaceSystem;
    if ((symbolic & kCTFontTraitMonoSpace) != 0U) {
        *flags = static_cast<std::uint16_t>(*flags | kFontFaceMonospace);
    }
    if ((symbolic & kCTFontTraitColorGlyphs) != 0U) {
        *flags = static_cast<std::uint16_t>(*flags | kFontFaceColor);
    }
    CfOwned<CFArrayRef> axes(CTFontCopyVariationAxes(font));
    if (axes && CFArrayGetCount(axes.get()) > 0) {
        *flags = static_cast<std::uint16_t>(*flags | kFontFaceVariable);
    }
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
        CfOwned<CTFontCollectionRef> collection(
            CTFontCollectionCreateFromAvailableFonts(nullptr));
        if (!collection) {
            return fail(
                CoreTextDiscoveryErrorKind::CollectionCreationFailed,
                0U,
                "CoreText failed to create the available-font collection",
                error);
        }
        CfOwned<CFArrayRef> descriptors(
            CTFontCollectionCreateMatchingFontDescriptors(collection.get()));
        if (!descriptors) {
            return fail(
                CoreTextDiscoveryErrorKind::EnumerationFailed,
                0U,
                "CoreText failed to enumerate normalized font descriptors",
                error);
        }
        const CFIndex descriptor_count = CFArrayGetCount(descriptors.get());
        if (descriptor_count <= 0 ||
            static_cast<std::uint64_t>(descriptor_count) >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
            return fail(
                CoreTextDiscoveryErrorKind::EnumerationFailed,
                0U,
                "CoreText returned an empty or oversized descriptor collection",
                error);
        }
        stats->descriptors_seen = static_cast<std::uint64_t>(descriptor_count);

        std::vector<OwnedFace> owned;
        owned.reserve(static_cast<std::size_t>(descriptor_count));
        for (CFIndex index = 0; index < descriptor_count; ++index) {
            const void* raw = CFArrayGetValueAtIndex(descriptors.get(), index);
            if (raw == nullptr ||
                CFGetTypeID(raw) != CTFontDescriptorGetTypeID()) {
                ++stats->descriptors_skipped;
                continue;
            }
            const auto descriptor = static_cast<CTFontDescriptorRef>(raw);
            CfOwned<CTFontRef> font(
                CTFontCreateWithFontDescriptor(descriptor, 0.0, nullptr));
            if (!font) {
                ++stats->descriptors_skipped;
                continue;
            }

            OwnedFace candidate;
            std::string path;
            std::string postscript;
            if (!descriptor_file_path(descriptor, &path) ||
                !font_names(font.get(), &postscript, &candidate.family) ||
                !font_metadata(
                    font.get(),
                    &candidate.weight,
                    &candidate.width,
                    &candidate.slant,
                    &candidate.flags)) {
                ++stats->descriptors_skipped;
                continue;
            }

            std::vector<VariationCoordinate> variations;
            if (!variation_coordinates(font.get(), &variations)) {
                ++stats->descriptors_skipped;
                continue;
            }

            CfOwned<CFCharacterSetRef> character_set(
                CTFontCopyCharacterSet(font.get()));
            std::uint64_t face_codepoints = 0U;
            std::uint64_t face_planes = 0U;
            if (!character_set ||
                !character_set_coverage(
                    character_set.get(),
                    &candidate.coverage,
                    &candidate.preferred_script,
                    &face_codepoints,
                    &face_planes)) {
                ++stats->descriptors_skipped;
                continue;
            }

            candidate.identity = make_identity(
                path,
                postscript,
                candidate.weight,
                candidate.width,
                candidate.slant,
                variations);
            stats->coverage_codepoints += face_codepoints;
            stats->coverage_ranges +=
                static_cast<std::uint64_t>(candidate.coverage.size());
            stats->bitmap_planes += face_planes;
            if ((candidate.flags & kFontFaceVariable) != 0U) {
                ++stats->variable_faces;
            }
            if ((candidate.flags & kFontFaceColor) != 0U) {
                ++stats->color_faces;
            }
            if ((candidate.flags & kFontFaceMonospace) != 0U) {
                ++stats->monospace_faces;
            }
            owned.push_back(std::move(candidate));
        }

        if (owned.empty()) {
            return fail(
                CoreTextDiscoveryErrorKind::EnumerationFailed,
                0U,
                "CoreText produced no addressable physical font faces",
                error);
        }
        std::sort(
            owned.begin(),
            owned.end(),
            [](const OwnedFace& left, const OwnedFace& right) {
                return left.identity < right.identity;
            });

        std::vector<OwnedFace> canonical;
        canonical.reserve(owned.size());
        for (OwnedFace& face : owned) {
            if (!canonical.empty() && canonical.back().identity == face.identity) {
                if (!same_face(canonical.back(), face)) {
                    return fail(
                        CoreTextDiscoveryErrorKind::ConflictingDuplicate,
                        0U,
                        "CoreText produced conflicting metadata for one face identity",
                        error);
                }
                ++stats->duplicate_faces_skipped;
                continue;
            }
            canonical.push_back(std::move(face));
        }

        std::vector<FontDiscoveryFace> views;
        views.reserve(canonical.size());
        for (const OwnedFace& face : canonical) {
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

        FontDiscoveryStats generation_stats;
        FontDiscoveryError generation_error;
        if (!build_font_catalog_generation(
                generation_id,
                views,
                discovery_hard_limit,
                catalog_hard_limit,
                output,
                &generation_stats,
                &generation_error)) {
            error->kind = CoreTextDiscoveryErrorKind::SnapshotBuildFailed;
            error->generation_error = generation_error.kind;
            error->catalog_error = generation_error.catalog_error;
            error->descriptor_index = generation_error.face_index;
            try {
                error->message = generation_error.message;
            } catch (...) {
                error->message.clear();
            }
            return false;
        }

        stats->faces_emitted = static_cast<std::uint64_t>(canonical.size());
        stats->generation = generation_stats;
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            CoreTextDiscoveryErrorKind::EnumerationFailed,
            0U,
            "CoreText discovery allocation failed",
            error);
    } catch (...) {
        return fail(
            CoreTextDiscoveryErrorKind::EnumerationFailed,
            0U,
            "CoreText discovery failed",
            error);
    }
}

} // namespace zevryon::text
