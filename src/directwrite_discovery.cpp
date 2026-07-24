#include "directwrite_discovery.hpp"

#include "unicode_script.hpp"

#include <windows.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <dwrite_2.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <climits>
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

using Microsoft::WRL::ComPtr;

constexpr std::size_t kScriptCount = static_cast<std::size_t>(ScriptId::Count);

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

void clear_error(DirectWriteDiscoveryError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    DirectWriteDiscoveryErrorKind kind,
    std::size_t family_index,
    std::size_t font_index,
    HRESULT native_error,
    const char* message,
    DirectWriteDiscoveryError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->family_index = family_index;
        error->font_index = font_index;
        error->native_error = static_cast<long>(native_error);
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool utf16_to_utf8(std::wstring_view input, std::string* output) {
    if (output == nullptr || input.empty() ||
        input.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    const int input_length = static_cast<int>(input.size());
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        input.data(),
        input_length,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return false;
    }
    output->resize(static_cast<std::size_t>(required));
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        input.data(),
        input_length,
        output->data(),
        required,
        nullptr,
        nullptr);
    return written == required;
}

bool localized_string(IDWriteLocalizedStrings* strings, std::string* output) {
    if (strings == nullptr || output == nullptr || strings->GetCount() == 0U) {
        return false;
    }

    UINT32 selected = 0U;
    BOOL exists = FALSE;
    HRESULT hr = strings->FindLocaleName(L"en-us", &selected, &exists);
    if (FAILED(hr)) {
        return false;
    }

    if (exists == FALSE) {
        std::wstring best_locale;
        bool have_best = false;
        const UINT32 count = strings->GetCount();
        for (UINT32 index = 0U; index < count; ++index) {
            UINT32 length = 0U;
            hr = strings->GetLocaleNameLength(index, &length);
            if (FAILED(hr) || length == std::numeric_limits<UINT32>::max()) {
                return false;
            }
            std::vector<wchar_t> locale(static_cast<std::size_t>(length) + 1U);
            hr = strings->GetLocaleName(index, locale.data(), length + 1U);
            if (FAILED(hr)) {
                return false;
            }
            const std::wstring candidate(locale.data(), length);
            if (!have_best || candidate < best_locale) {
                best_locale = candidate;
                selected = index;
                have_best = true;
            }
        }
        if (!have_best) {
            return false;
        }
    }

    UINT32 length = 0U;
    hr = strings->GetStringLength(selected, &length);
    if (FAILED(hr) || length == 0U ||
        length == std::numeric_limits<UINT32>::max()) {
        return false;
    }
    std::vector<wchar_t> value(static_cast<std::size_t>(length) + 1U);
    hr = strings->GetString(selected, value.data(), length + 1U);
    return SUCCEEDED(hr) &&
           utf16_to_utf8(std::wstring_view(value.data(), length), output);
}

bool family_name(IDWriteFontFamily* family, std::string* output) {
    ComPtr<IDWriteLocalizedStrings> names;
    return family != nullptr &&
           SUCCEEDED(family->GetFamilyNames(names.GetAddressOf())) &&
           localized_string(names.Get(), output);
}

bool postscript_name(IDWriteFont* font, std::string* output) {
    if (font == nullptr || output == nullptr) {
        return false;
    }
    output->clear();
    ComPtr<IDWriteLocalizedStrings> names;
    BOOL exists = FALSE;
    const HRESULT hr = font->GetInformationalStrings(
        DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME,
        names.GetAddressOf(),
        &exists);
    if (FAILED(hr)) {
        return false;
    }
    return exists == FALSE || localized_string(names.Get(), output);
}

void append_identity_field(std::string* output, std::string_view value) {
    output->append(std::to_string(value.size()));
    output->push_back(':');
    output->append(value);
    output->push_back('|');
}

bool local_file_path(IDWriteFontFile* file, std::string* output) {
    if (file == nullptr || output == nullptr) {
        return false;
    }

    const void* reference_key = nullptr;
    UINT32 reference_key_size = 0U;
    HRESULT hr = file->GetReferenceKey(&reference_key, &reference_key_size);
    if (FAILED(hr) || reference_key == nullptr || reference_key_size == 0U) {
        return false;
    }

    ComPtr<IDWriteFontFileLoader> loader;
    hr = file->GetLoader(loader.GetAddressOf());
    if (FAILED(hr)) {
        return false;
    }
    ComPtr<IDWriteLocalFontFileLoader> local_loader;
    hr = loader.As(&local_loader);
    if (FAILED(hr)) {
        return false;
    }

    UINT32 path_length = 0U;
    hr = local_loader->GetFilePathLengthFromKey(
        reference_key,
        reference_key_size,
        &path_length);
    if (FAILED(hr) || path_length == 0U ||
        path_length == std::numeric_limits<UINT32>::max()) {
        return false;
    }

    std::vector<wchar_t> path(static_cast<std::size_t>(path_length) + 1U);
    hr = local_loader->GetFilePathFromKey(
        reference_key,
        reference_key_size,
        path.data(),
        path_length + 1U);
    return SUCCEEDED(hr) &&
           utf16_to_utf8(std::wstring_view(path.data(), path_length), output);
}

void release_raw_files(std::vector<IDWriteFontFile*>* files) noexcept {
    for (IDWriteFontFile*& file : *files) {
        if (file != nullptr) {
            file->Release();
            file = nullptr;
        }
    }
}

bool face_file_paths(IDWriteFontFace* face, std::vector<std::string>* output) {
    if (face == nullptr || output == nullptr) {
        return false;
    }
    UINT32 count = 0U;
    HRESULT hr = face->GetFiles(&count, nullptr);
    if (FAILED(hr) || count == 0U) {
        return false;
    }

    std::vector<IDWriteFontFile*> raw_files(
        static_cast<std::size_t>(count),
        nullptr);
    std::vector<ComPtr<IDWriteFontFile>> files(
        static_cast<std::size_t>(count));
    hr = face->GetFiles(&count, raw_files.data());
    if (FAILED(hr) || static_cast<std::size_t>(count) > raw_files.size()) {
        release_raw_files(&raw_files);
        return false;
    }

    for (std::size_t index = 0U;
         index < static_cast<std::size_t>(count);
         ++index) {
        if (raw_files[index] == nullptr) {
            release_raw_files(&raw_files);
            return false;
        }
        files[index].Attach(raw_files[index]);
        raw_files[index] = nullptr;
    }
    release_raw_files(&raw_files);

    output->clear();
    output->reserve(static_cast<std::size_t>(count));
    for (const ComPtr<IDWriteFontFile>& file : files) {
        std::string path;
        if (!local_file_path(file.Get(), &path)) {
            return false;
        }
        output->push_back(std::move(path));
    }

    std::sort(output->begin(), output->end());
    output->erase(std::unique(output->begin(), output->end()), output->end());
    return !output->empty();
}

std::string make_identity(
    const std::vector<std::string>& paths,
    DWRITE_FONT_FACE_TYPE face_type,
    UINT32 face_index,
    DWRITE_FONT_WEIGHT weight,
    DWRITE_FONT_STRETCH stretch,
    DWRITE_FONT_STYLE style,
    std::string_view postscript) {
    std::string identity;
    std::size_t path_bytes = 0U;
    for (const std::string& path : paths) {
        if (path.size() > std::numeric_limits<std::size_t>::max() - path_bytes) {
            path_bytes = 0U;
            break;
        }
        path_bytes += path.size();
    }
    const std::size_t fixed_estimate = 96U;
    if (path_bytes <= std::numeric_limits<std::size_t>::max() - fixed_estimate &&
        postscript.size() <=
            std::numeric_limits<std::size_t>::max() - fixed_estimate - path_bytes) {
        identity.reserve(fixed_estimate + path_bytes + postscript.size());
    }
    identity.append("directwrite|");
    append_identity_field(&identity, std::to_string(paths.size()));
    for (const std::string& path : paths) {
        append_identity_field(&identity, path);
    }
    append_identity_field(
        &identity,
        std::to_string(static_cast<unsigned>(face_type)));
    append_identity_field(&identity, std::to_string(face_index));
    append_identity_field(
        &identity,
        std::to_string(static_cast<unsigned>(weight)));
    append_identity_field(
        &identity,
        std::to_string(static_cast<unsigned>(stretch)));
    append_identity_field(
        &identity,
        std::to_string(static_cast<unsigned>(style)));
    append_identity_field(&identity, postscript);
    return identity;
}

std::uint16_t css_weight(DWRITE_FONT_WEIGHT weight) noexcept {
    const unsigned value = static_cast<unsigned>(weight);
    return static_cast<std::uint16_t>(std::clamp(value, 1U, 1000U));
}

std::uint8_t css_width(DWRITE_FONT_STRETCH stretch) noexcept {
    const unsigned value = static_cast<unsigned>(stretch);
    return value >= 1U && value <= 9U
        ? static_cast<std::uint8_t>(value)
        : static_cast<std::uint8_t>(5U);
}

FontSlant font_slant(DWRITE_FONT_STYLE style) noexcept {
    if (style == DWRITE_FONT_STYLE_ITALIC) {
        return FontSlant::Italic;
    }
    if (style == DWRITE_FONT_STYLE_OBLIQUE) {
        return FontSlant::Oblique;
    }
    return FontSlant::Upright;
}

bool append_unicode_ranges(
    IDWriteFontFace1* face,
    std::vector<FontCoverageRange>* output,
    ScriptId* preferred_script,
    std::uint64_t* codepoint_count) {
    if (face == nullptr || output == nullptr || preferred_script == nullptr ||
        codepoint_count == nullptr) {
        return false;
    }

    UINT32 actual = 0U;
    HRESULT hr = face->GetUnicodeRanges(0U, nullptr, &actual);
    if ((hr != S_OK && hr != E_NOT_SUFFICIENT_BUFFER) || actual == 0U) {
        return false;
    }

    std::vector<DWRITE_UNICODE_RANGE> native_ranges(
        static_cast<std::size_t>(actual));
    bool complete = false;
    for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
        if (native_ranges.size() > static_cast<std::size_t>(
                                       std::numeric_limits<UINT32>::max())) {
            return false;
        }
        UINT32 returned = 0U;
        hr = face->GetUnicodeRanges(
            static_cast<UINT32>(native_ranges.size()),
            native_ranges.data(),
            &returned);
        if (hr == E_NOT_SUFFICIENT_BUFFER &&
            static_cast<std::size_t>(returned) > native_ranges.size()) {
            native_ranges.resize(static_cast<std::size_t>(returned));
            continue;
        }
        if (FAILED(hr) || returned == 0U ||
            static_cast<std::size_t>(returned) > native_ranges.size()) {
            return false;
        }
        native_ranges.resize(static_cast<std::size_t>(returned));
        complete = true;
        break;
    }
    if (!complete) {
        return false;
    }

    std::sort(
        native_ranges.begin(),
        native_ranges.end(),
        [](const DWRITE_UNICODE_RANGE& left, const DWRITE_UNICODE_RANGE& right) {
            return left.first < right.first ||
                   (left.first == right.first && left.last < right.last);
        });

    output->clear();
    output->reserve(native_ranges.size());
    for (const DWRITE_UNICODE_RANGE range : native_ranges) {
        if (range.first > range.last || range.last > 0x10ffffU) {
            return false;
        }
        if (output->empty() ||
            static_cast<std::uint64_t>(range.first) >
                static_cast<std::uint64_t>(output->back().last) + 1U) {
            output->push_back({range.first, range.last});
        } else {
            output->back().last = std::max(output->back().last, range.last);
        }
    }
    if (output->empty()) {
        return false;
    }

    std::array<std::uint64_t, kScriptCount> script_counts{};
    std::uint64_t total = 0U;
    for (const FontCoverageRange range : *output) {
        total += static_cast<std::uint64_t>(range.last) -
                 static_cast<std::uint64_t>(range.first) + 1U;
        for (std::uint64_t value = range.first; value <= range.last; ++value) {
            const ScriptId script = script_of(static_cast<std::uint32_t>(value));
            if (!is_neutral_script(script)) {
                const std::size_t index = static_cast<std::size_t>(script);
                if (index < script_counts.size()) {
                    ++script_counts[index];
                }
            }
        }
    }

    const auto best = std::max_element(script_counts.begin(), script_counts.end());
    *preferred_script = best != script_counts.end() && *best != 0U
        ? static_cast<ScriptId>(
              static_cast<std::size_t>(best - script_counts.begin()))
        : ScriptId::Zyyy;
    *codepoint_count = total;
    return true;
}

std::uint16_t face_flags(IDWriteFont* font, IDWriteFontFace* face) noexcept {
    std::uint16_t flags = kFontFaceSystem;
    if (font == nullptr || face == nullptr) {
        return flags;
    }

    ComPtr<IDWriteFont1> font1;
    if (SUCCEEDED(font->QueryInterface(
            IID_PPV_ARGS(font1.ReleaseAndGetAddressOf()))) &&
        font1->IsMonospacedFont() != FALSE) {
        flags = static_cast<std::uint16_t>(flags | kFontFaceMonospace);
    }

    ComPtr<IDWriteFont2> font2;
    if (SUCCEEDED(font->QueryInterface(
            IID_PPV_ARGS(font2.ReleaseAndGetAddressOf()))) &&
        font2->IsColorFont() != FALSE) {
        flags = static_cast<std::uint16_t>(flags | kFontFaceColor);
    }

    ComPtr<IDWriteFontFace5> face5;
    if (SUCCEEDED(face->QueryInterface(
            IID_PPV_ARGS(face5.ReleaseAndGetAddressOf()))) &&
        face5->HasVariations() != FALSE) {
        flags = static_cast<std::uint16_t>(flags | kFontFaceVariable);
    }
    return flags;
}

bool same_face(const OwnedFace& left, const OwnedFace& right) noexcept {
    return left.identity == right.identity && left.family == right.family &&
           left.weight == right.weight && left.width == right.width &&
           left.slant == right.slant &&
           left.preferred_script == right.preferred_script &&
           left.flags == right.flags && left.coverage == right.coverage;
}

} // namespace

const char* directwrite_discovery_error_kind_name(
    DirectWriteDiscoveryErrorKind kind) noexcept {
    switch (kind) {
        case DirectWriteDiscoveryErrorKind::None:
            return "none";
        case DirectWriteDiscoveryErrorKind::FactoryCreationFailed:
            return "factory_creation_failed";
        case DirectWriteDiscoveryErrorKind::EnumerationFailed:
            return "enumeration_failed";
        case DirectWriteDiscoveryErrorKind::MissingRequiredProperty:
            return "missing_required_property";
        case DirectWriteDiscoveryErrorKind::UnsupportedFontFile:
            return "unsupported_font_file";
        case DirectWriteDiscoveryErrorKind::InvalidNativeValue:
            return "invalid_native_value";
        case DirectWriteDiscoveryErrorKind::ConflictingDuplicate:
            return "conflicting_duplicate";
        case DirectWriteDiscoveryErrorKind::SnapshotBuildFailed:
            return "snapshot_build_failed";
    }
    return "invalid";
}

bool build_directwrite_generation(
    std::uint64_t generation_id,
    std::size_t discovery_hard_limit,
    std::size_t catalog_hard_limit,
    std::shared_ptr<const FontCatalogGeneration>* output,
    DirectWriteDiscoveryStats* stats,
    DirectWriteDiscoveryError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->reset();
    *stats = {};
    clear_error(error);

    try {
        ComPtr<IDWriteFactory> factory;
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            return fail(
                DirectWriteDiscoveryErrorKind::FactoryCreationFailed,
                0U,
                0U,
                hr,
                "DirectWrite factory creation failed",
                error);
        }

        ComPtr<IDWriteFontCollection> collection;
        hr = factory->GetSystemFontCollection(collection.GetAddressOf(), FALSE);
        if (FAILED(hr)) {
            return fail(
                DirectWriteDiscoveryErrorKind::EnumerationFailed,
                0U,
                0U,
                hr,
                "DirectWrite system font collection enumeration failed",
                error);
        }

        std::vector<OwnedFace> owned;
        const UINT32 family_count = collection->GetFontFamilyCount();
        stats->families_seen = static_cast<std::uint64_t>(family_count);

        for (UINT32 family_index = 0U; family_index < family_count; ++family_index) {
            ComPtr<IDWriteFontFamily> family;
            hr = collection->GetFontFamily(family_index, family.GetAddressOf());
            if (FAILED(hr)) {
                return fail(
                    DirectWriteDiscoveryErrorKind::EnumerationFailed,
                    static_cast<std::size_t>(family_index),
                    0U,
                    hr,
                    "DirectWrite failed to retrieve a font family",
                    error);
            }

            std::string family_utf8;
            if (!family_name(family.Get(), &family_utf8)) {
                return fail(
                    DirectWriteDiscoveryErrorKind::MissingRequiredProperty,
                    static_cast<std::size_t>(family_index),
                    0U,
                    E_FAIL,
                    "DirectWrite family has no deterministic UTF-8 name",
                    error);
            }

            const UINT32 font_count = family->GetFontCount();
            for (UINT32 font_index = 0U; font_index < font_count; ++font_index) {
                ++stats->fonts_seen;
                ComPtr<IDWriteFont> font;
                hr = family->GetFont(font_index, font.GetAddressOf());
                if (FAILED(hr)) {
                    return fail(
                        DirectWriteDiscoveryErrorKind::EnumerationFailed,
                        static_cast<std::size_t>(family_index),
                        static_cast<std::size_t>(font_index),
                        hr,
                        "DirectWrite failed to retrieve a physical font",
                        error);
                }

                if (font->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE) {
                    ++stats->simulated_fonts_skipped;
                    continue;
                }

                ComPtr<IDWriteFontFace> face;
                hr = font->CreateFontFace(face.GetAddressOf());
                if (FAILED(hr)) {
                    return fail(
                        DirectWriteDiscoveryErrorKind::EnumerationFailed,
                        static_cast<std::size_t>(family_index),
                        static_cast<std::size_t>(font_index),
                        hr,
                        "DirectWrite failed to create a font face",
                        error);
                }

                ComPtr<IDWriteFontFace1> face1;
                hr = face.As(&face1);
                if (FAILED(hr)) {
                    return fail(
                        DirectWriteDiscoveryErrorKind::InvalidNativeValue,
                        static_cast<std::size_t>(family_index),
                        static_cast<std::size_t>(font_index),
                        hr,
                        "DirectWrite font face does not expose Unicode ranges",
                        error);
                }

                OwnedFace candidate;
                candidate.family = family_utf8;
                std::uint64_t face_codepoints = 0U;
                if (!append_unicode_ranges(
                        face1.Get(),
                        &candidate.coverage,
                        &candidate.preferred_script,
                        &face_codepoints)) {
                    return fail(
                        DirectWriteDiscoveryErrorKind::InvalidNativeValue,
                        static_cast<std::size_t>(family_index),
                        static_cast<std::size_t>(font_index),
                        E_FAIL,
                        "DirectWrite returned invalid or empty Unicode coverage",
                        error);
                }

                std::vector<std::string> paths;
                if (!face_file_paths(face.Get(), &paths)) {
                    return fail(
                        DirectWriteDiscoveryErrorKind::UnsupportedFontFile,
                        static_cast<std::size_t>(family_index),
                        static_cast<std::size_t>(font_index),
                        E_NOINTERFACE,
                        "DirectWrite font files are not addressable by the local loader",
                        error);
                }

                std::string postscript;
                if (!postscript_name(font.Get(), &postscript)) {
                    return fail(
                        DirectWriteDiscoveryErrorKind::InvalidNativeValue,
                        static_cast<std::size_t>(family_index),
                        static_cast<std::size_t>(font_index),
                        E_FAIL,
                        "DirectWrite returned an invalid PostScript name",
                        error);
                }

                const DWRITE_FONT_WEIGHT native_weight = font->GetWeight();
                const DWRITE_FONT_STRETCH native_stretch = font->GetStretch();
                const DWRITE_FONT_STYLE native_style = font->GetStyle();
                candidate.identity = make_identity(
                    paths,
                    face->GetType(),
                    face->GetIndex(),
                    native_weight,
                    native_stretch,
                    native_style,
                    postscript);
                candidate.weight = css_weight(native_weight);
                candidate.width = css_width(native_stretch);
                candidate.slant = font_slant(native_style);
                candidate.flags = face_flags(font.Get(), face.Get());

                stats->coverage_codepoints += face_codepoints;
                stats->coverage_ranges +=
                    static_cast<std::uint64_t>(candidate.coverage.size());
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
        }

        if (owned.empty()) {
            return fail(
                DirectWriteDiscoveryErrorKind::EnumerationFailed,
                0U,
                0U,
                E_FAIL,
                "DirectWrite system collection produced no physical faces",
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
                        DirectWriteDiscoveryErrorKind::ConflictingDuplicate,
                        0U,
                        0U,
                        E_FAIL,
                        "DirectWrite produced conflicting metadata for one face identity",
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
            error->kind = DirectWriteDiscoveryErrorKind::SnapshotBuildFailed;
            error->generation_error = generation_error.kind;
            error->catalog_error = generation_error.catalog_error;
            error->family_index = generation_error.face_index;
            error->font_index = 0U;
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
            DirectWriteDiscoveryErrorKind::EnumerationFailed,
            0U,
            0U,
            E_OUTOFMEMORY,
            "DirectWrite discovery allocation failed",
            error);
    } catch (...) {
        return fail(
            DirectWriteDiscoveryErrorKind::EnumerationFailed,
            0U,
            0U,
            E_FAIL,
            "DirectWrite discovery failed",
            error);
    }
}

} // namespace zevryon::text
