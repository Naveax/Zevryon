#include "directwrite_discovery.hpp"

#include "unicode_script.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
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

struct FontFileArray {
    std::vector<IDWriteFontFile*> values;

    ~FontFileArray() {
        for (IDWriteFontFile* value : values) {
            if (value != nullptr) {
                value->Release();
            }
        }
    }
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
    HRESULT native_result,
    const char* message,
    DirectWriteDiscoveryError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->family_index = family_index;
        error->font_index = font_index;
        error->native_result = static_cast<long>(native_result);
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool wide_to_utf8(std::wstring_view input, std::string* output) {
    if (output == nullptr || input.empty() ||
        input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int input_size = static_cast<int>(input.size());
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        input.data(),
        input_size,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return false;
    }
    output->assign(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        input.data(),
        input_size,
        output->data(),
        required,
        nullptr,
        nullptr);
    return written == required;
}

bool localized_name(IDWriteLocalizedStrings* names, std::string* output) {
    if (names == nullptr || output == nullptr || names->GetCount() == 0U) {
        return false;
    }
    UINT32 index = 0U;
    BOOL exists = FALSE;
    HRESULT result = names->FindLocaleName(L"en-us", &index, &exists);
    if (FAILED(result)) {
        return false;
    }
    if (exists == FALSE) {
        index = 0U;
    }
    UINT32 length = 0U;
    result = names->GetStringLength(index, &length);
    if (FAILED(result) || length == 0U) {
        return false;
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1U, L'\0');
    result = names->GetString(index, buffer.data(), length + 1U);
    if (FAILED(result)) {
        return false;
    }
    return wide_to_utf8(std::wstring_view(buffer.data(), length), output);
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

bool local_file_path(IDWriteFontFile* file, std::string* output) {
    if (file == nullptr || output == nullptr) {
        return false;
    }
    const void* reference_key = nullptr;
    UINT32 reference_key_size = 0U;
    HRESULT result = file->GetReferenceKey(&reference_key, &reference_key_size);
    if (FAILED(result) || reference_key == nullptr || reference_key_size == 0U) {
        return false;
    }
    ComPtr<IDWriteFontFileLoader> loader;
    result = file->GetLoader(loader.GetAddressOf());
    if (FAILED(result) || !loader) {
        return false;
    }
    ComPtr<IDWriteLocalFontFileLoader> local_loader;
    result = loader.As(&local_loader);
    if (FAILED(result) || !local_loader) {
        return false;
    }
    UINT32 path_length = 0U;
    result = local_loader->GetFilePathLengthFromKey(
        reference_key,
        reference_key_size,
        &path_length);
    if (FAILED(result) || path_length == 0U) {
        return false;
    }
    std::vector<wchar_t> path(static_cast<std::size_t>(path_length) + 1U, L'\0');
    result = local_loader->GetFilePathFromKey(
        reference_key,
        reference_key_size,
        path.data(),
        path_length + 1U);
    if (FAILED(result)) {
        return false;
    }
    return wide_to_utf8(std::wstring_view(path.data(), path_length), output);
}

bool face_file_paths(
    IDWriteFontFace* face,
    std::vector<std::string>* paths,
    std::uint64_t* files_seen) {
    if (face == nullptr || paths == nullptr || files_seen == nullptr) {
        return false;
    }
    UINT32 file_count = 0U;
    HRESULT result = face->GetFiles(&file_count, nullptr);
    if (FAILED(result) || file_count == 0U) {
        return false;
    }
    FontFileArray files;
    files.values.assign(static_cast<std::size_t>(file_count), nullptr);
    result = face->GetFiles(&file_count, files.values.data());
    if (FAILED(result)) {
        return false;
    }
    paths->clear();
    paths->reserve(static_cast<std::size_t>(file_count));
    for (IDWriteFontFile* file : files.values) {
        std::string path;
        if (!local_file_path(file, &path)) {
            return false;
        }
        paths->push_back(std::move(path));
    }
    std::sort(paths->begin(), paths->end());
    if (std::adjacent_find(paths->begin(), paths->end()) != paths->end()) {
        paths->erase(std::unique(paths->begin(), paths->end()), paths->end());
    }
    *files_seen += static_cast<std::uint64_t>(paths->size());
    return !paths->empty();
}

std::uint16_t css_weight(DWRITE_FONT_WEIGHT weight) noexcept {
    const int value = static_cast<int>(weight);
    return static_cast<std::uint16_t>(std::clamp(value, 1, 1000));
}

std::uint8_t css_width(DWRITE_FONT_STRETCH stretch) noexcept {
    const int value = static_cast<int>(stretch);
    if (value < static_cast<int>(DWRITE_FONT_STRETCH_ULTRA_CONDENSED) ||
        value > static_cast<int>(DWRITE_FONT_STRETCH_ULTRA_EXPANDED)) {
        return 5U;
    }
    return static_cast<std::uint8_t>(value);
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

bool canonical_coverage(
    IDWriteFontFace* face,
    std::vector<FontCoverageRange>* ranges,
    ScriptId* preferred_script,
    std::uint64_t* codepoint_count) {
    if (face == nullptr || ranges == nullptr || preferred_script == nullptr ||
        codepoint_count == nullptr) {
        return false;
    }
    ComPtr<IDWriteFontFace1> face1;
    HRESULT result = face->QueryInterface(IID_PPV_ARGS(face1.GetAddressOf()));
    if (FAILED(result) || !face1) {
        return false;
    }
    UINT32 range_count = 0U;
    result = face1->GetUnicodeRanges(0U, nullptr, &range_count);
    if (result != E_NOT_SUFFICIENT_BUFFER && FAILED(result)) {
        return false;
    }
    if (range_count == 0U) {
        return false;
    }
    std::vector<DWRITE_UNICODE_RANGE> native_ranges(range_count);
    result = face1->GetUnicodeRanges(
        range_count,
        native_ranges.data(),
        &range_count);
    if (FAILED(result) || range_count == 0U) {
        return false;
    }
    native_ranges.resize(range_count);
    std::sort(
        native_ranges.begin(),
        native_ranges.end(),
        [](const DWRITE_UNICODE_RANGE& left, const DWRITE_UNICODE_RANGE& right) {
            return left.first < right.first ||
                (left.first == right.first && left.last < right.last);
        });

    std::array<std::uint64_t, kScriptCount> script_counts{};
    ranges->clear();
    for (const DWRITE_UNICODE_RANGE& native : native_ranges) {
        if (native.first > native.last || native.last > 0x10ffffU) {
            return false;
        }
        FontCoverageRange next{native.first, native.last};
        if (!ranges->empty() && next.first <= ranges->back().last + 1U) {
            ranges->back().last = std::max(ranges->back().last, next.last);
        } else {
            ranges->push_back(next);
        }
    }
    if (ranges->empty()) {
        return false;
    }

    *codepoint_count = 0U;
    for (const FontCoverageRange& range : *ranges) {
        *codepoint_count +=
            static_cast<std::uint64_t>(range.last) -
            static_cast<std::uint64_t>(range.first) + 1U;
        for (std::uint32_t codepoint = range.first;; ++codepoint) {
            const ScriptId script = script_of(codepoint);
            if (!is_neutral_script(script)) {
                const std::size_t index = static_cast<std::size_t>(script);
                if (index < script_counts.size()) {
                    ++script_counts[index];
                }
            }
            if (codepoint == range.last) {
                break;
            }
        }
    }
    const auto best = std::max_element(script_counts.begin(), script_counts.end());
    *preferred_script = best != script_counts.end() && *best != 0U
        ? static_cast<ScriptId>(static_cast<std::size_t>(best - script_counts.begin()))
        : ScriptId::Zyyy;
    return true;
}

std::string make_identity(
    const std::vector<std::string>& paths,
    UINT32 face_index,
    std::string_view family,
    std::string_view face_name,
    DWRITE_FONT_WEIGHT weight,
    DWRITE_FONT_STRETCH stretch,
    DWRITE_FONT_STYLE style,
    DWRITE_FONT_SIMULATIONS simulations) {
    std::string identity;
    std::size_t reserve = 96U + family.size() + face_name.size();
    for (const std::string& path : paths) {
        reserve += path.size() + 24U;
    }
    identity.reserve(reserve);
    identity.append("directwrite|");
    append_identity_integer(&identity, static_cast<std::uint64_t>(paths.size()));
    for (const std::string& path : paths) {
        append_identity_field(&identity, path);
    }
    append_identity_integer(&identity, face_index);
    append_identity_field(&identity, family);
    append_identity_field(&identity, face_name);
    append_identity_integer(&identity, static_cast<std::uint64_t>(weight));
    append_identity_integer(&identity, static_cast<std::uint64_t>(stretch));
    append_identity_integer(&identity, static_cast<std::uint64_t>(style));
    append_identity_integer(&identity, static_cast<std::uint64_t>(simulations));
    return identity;
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
        case DirectWriteDiscoveryErrorKind::InvalidNativeValue:
            return "invalid_native_value";
        case DirectWriteDiscoveryErrorKind::UnsupportedFontSource:
            return "unsupported_font_source";
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
        HRESULT result = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
        if (FAILED(result) || !factory) {
            return fail(
                DirectWriteDiscoveryErrorKind::FactoryCreationFailed,
                0U,
                0U,
                result,
                "DirectWrite factory creation failed",
                error);
        }

        ComPtr<IDWriteFontCollection> collection;
        result = factory->GetSystemFontCollection(collection.GetAddressOf(), TRUE);
        if (FAILED(result) || !collection) {
            return fail(
                DirectWriteDiscoveryErrorKind::EnumerationFailed,
                0U,
                0U,
                result,
                "DirectWrite system font collection enumeration failed",
                error);
        }

        const UINT32 family_count = collection->GetFontFamilyCount();
        stats->families_seen = static_cast<std::uint64_t>(family_count);
        std::vector<OwnedFace> owned;

        for (UINT32 family_index = 0U; family_index < family_count; ++family_index) {
            ComPtr<IDWriteFontFamily> family;
            result = collection->GetFontFamily(family_index, family.GetAddressOf());
            if (FAILED(result) || !family) {
                return fail(
                    DirectWriteDiscoveryErrorKind::EnumerationFailed,
                    family_index,
                    0U,
                    result,
                    "DirectWrite failed to obtain a font family",
                    error);
            }
            ComPtr<IDWriteLocalizedStrings> family_names;
            result = family->GetFamilyNames(family_names.GetAddressOf());
            std::string family_name;
            if (FAILED(result) || !localized_name(family_names.Get(), &family_name)) {
                return fail(
                    DirectWriteDiscoveryErrorKind::MissingRequiredProperty,
                    family_index,
                    0U,
                    result,
                    "DirectWrite font family lacks a usable localized name",
                    error);
            }

            const UINT32 font_count = family->GetFontCount();
            stats->fonts_seen += static_cast<std::uint64_t>(font_count);
            owned.reserve(owned.size() + static_cast<std::size_t>(font_count));
            for (UINT32 font_index = 0U; font_index < font_count; ++font_index) {
                ComPtr<IDWriteFont> font;
                result = family->GetFont(font_index, font.GetAddressOf());
                if (FAILED(result) || !font) {
                    return fail(
                        DirectWriteDiscoveryErrorKind::EnumerationFailed,
                        family_index,
                        font_index,
                        result,
                        "DirectWrite failed to obtain a font",
                        error);
                }
                ComPtr<IDWriteLocalizedStrings> face_names;
                result = font->GetFaceNames(face_names.GetAddressOf());
                std::string face_name;
                if (FAILED(result) || !localized_name(face_names.Get(), &face_name)) {
                    return fail(
                        DirectWriteDiscoveryErrorKind::MissingRequiredProperty,
                        family_index,
                        font_index,
                        result,
                        "DirectWrite font lacks a usable face name",
                        error);
                }
                ComPtr<IDWriteFontFace> face;
                result = font->CreateFontFace(face.GetAddressOf());
                if (FAILED(result) || !face) {
                    return fail(
                        DirectWriteDiscoveryErrorKind::EnumerationFailed,
                        family_index,
                        font_index,
                        result,
                        "DirectWrite failed to create a font face",
                        error);
                }

                std::vector<std::string> paths;
                if (!face_file_paths(face.Get(), &paths, &stats->font_files_seen)) {
                    return fail(
                        DirectWriteDiscoveryErrorKind::UnsupportedFontSource,
                        family_index,
                        font_index,
                        E_NOINTERFACE,
                        "DirectWrite system font is not backed by readable local files",
                        error);
                }

                OwnedFace owned_face;
                owned_face.family = family_name;
                owned_face.weight = css_weight(font->GetWeight());
                owned_face.width = css_width(font->GetStretch());
                owned_face.slant = font_slant(font->GetStyle());
                const DWRITE_FONT_SIMULATIONS simulations = font->GetSimulations();
                if (simulations != DWRITE_FONT_SIMULATIONS_NONE) {
                    ++stats->simulated_faces;
                }
                owned_face.identity = make_identity(
                    paths,
                    face->GetIndex(),
                    family_name,
                    face_name,
                    font->GetWeight(),
                    font->GetStretch(),
                    font->GetStyle(),
                    simulations);
                owned_face.flags = kFontFaceSystem;

                ComPtr<IDWriteFont1> font1;
                if (SUCCEEDED(font.As(&font1)) && font1 &&
                    font1->IsMonospacedFont() != FALSE) {
                    owned_face.flags = static_cast<std::uint16_t>(
                        owned_face.flags | kFontFaceMonospace);
                    ++stats->monospace_faces;
                }

                std::uint64_t coverage_codepoints = 0U;
                if (!canonical_coverage(
                        face.Get(),
                        &owned_face.coverage,
                        &owned_face.preferred_script,
                        &coverage_codepoints)) {
                    return fail(
                        DirectWriteDiscoveryErrorKind::InvalidNativeValue,
                        family_index,
                        font_index,
                        E_FAIL,
                        "DirectWrite returned empty or invalid Unicode coverage",
                        error);
                }
                stats->coverage_codepoints += coverage_codepoints;
                stats->coverage_ranges += static_cast<std::uint64_t>(
                    owned_face.coverage.size());
                owned.push_back(std::move(owned_face));
            }
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
                        DirectWriteDiscoveryErrorKind::ConflictingDuplicate,
                        unique.size(),
                        0U,
                        E_FAIL,
                        "DirectWrite returned conflicting metadata for one identity",
                        error);
                }
                ++stats->duplicate_faces_skipped;
                continue;
            }
            unique.push_back(std::move(face));
        }
        if (unique.empty()) {
            return fail(
                DirectWriteDiscoveryErrorKind::EnumerationFailed,
                0U,
                0U,
                E_FAIL,
                "DirectWrite system font collection is empty",
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
            error->kind = DirectWriteDiscoveryErrorKind::SnapshotBuildFailed;
            error->generation_error = generation_error.kind;
            error->catalog_error = generation_error.catalog_error;
            error->message = generation_error.message;
            return false;
        }
        stats->faces_emitted = static_cast<std::uint64_t>(unique.size());
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            DirectWriteDiscoveryErrorKind::EnumerationFailed,
            0U,
            0U,
            E_OUTOFMEMORY,
            "DirectWrite adapter allocation failed",
            error);
    } catch (...) {
        return fail(
            DirectWriteDiscoveryErrorKind::EnumerationFailed,
            0U,
            0U,
            E_FAIL,
            "DirectWrite adapter failed",
            error);
    }
}

} // namespace zevryon::text
