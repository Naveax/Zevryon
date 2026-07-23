#include "font_catalog.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::uint32_t kMaximumUnicodeCodePoint = 0x10ffffU;
constexpr std::uint16_t kKnownFontFaceFlags =
    kFontFaceVariable | kFontFaceColor | kFontFaceMonospace | kFontFaceSystem;

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

void clear_error(FontCatalogError* error) noexcept {
    if (error != nullptr) {
        error->kind = FontCatalogErrorKind::None;
        error->face_index = 0U;
        error->message.clear();
    }
}

bool fail(
    FontCatalogErrorKind kind,
    std::size_t face_index,
    const char* message,
    FontCatalogError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->face_index = face_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool valid_slant(FontSlant slant) noexcept {
    return slant == FontSlant::Upright ||
           slant == FontSlant::Italic ||
           slant == FontSlant::Oblique;
}

bool validate_seed(
    const FontFaceSeed& seed,
    std::size_t face_index,
    FontCatalogError* error) noexcept {
    if (seed.stable_key == 0U || seed.weight == 0U || seed.weight > 1000U ||
        seed.width == 0U || seed.width > 9U || !valid_slant(seed.slant) ||
        (seed.flags & static_cast<std::uint16_t>(~kKnownFontFaceFlags)) != 0U ||
        seed.coverage.empty()) {
        return fail(
            FontCatalogErrorKind::InvalidInput,
            face_index,
            "font face metadata or coverage is invalid",
            error);
    }

    std::uint32_t previous_last = 0U;
    bool have_previous = false;
    for (const FontCoverageRange range : seed.coverage) {
        if (range.first > range.last || range.last > kMaximumUnicodeCodePoint ||
            (have_previous && range.first <= previous_last)) {
            return fail(
                FontCatalogErrorKind::InvalidInput,
                face_index,
                "font coverage ranges must be sorted, non-overlapping Unicode ranges",
                error);
        }
        previous_last = range.last;
        have_previous = true;
    }
    return true;
}

std::size_t canonical_range_count(std::span<const FontCoverageRange> ranges) noexcept {
    std::size_t count = 0U;
    std::uint32_t previous_last = 0U;
    bool have_previous = false;
    for (const FontCoverageRange range : ranges) {
        const bool adjacent =
            have_previous && previous_last != kMaximumUnicodeCodePoint &&
            range.first == previous_last + 1U;
        if (!adjacent) {
            ++count;
        }
        previous_last = range.last;
        have_previous = true;
    }
    return count;
}

} // namespace

FontCatalog::FontCatalog(std::pmr::memory_resource* resource)
    : faces(resource), coverage_ranges(resource) {}

std::pmr::memory_resource* FontCatalog::resource() const noexcept {
    return faces.get_allocator().resource();
}

void FontCatalog::release() noexcept {
    release_vector(&faces);
    release_vector(&coverage_ranges);
}

const char* font_catalog_error_kind_name(FontCatalogErrorKind kind) noexcept {
    switch (kind) {
        case FontCatalogErrorKind::None:
            return "none";
        case FontCatalogErrorKind::InvalidInput:
            return "invalid_input";
        case FontCatalogErrorKind::DuplicateStableKey:
            return "duplicate_stable_key";
        case FontCatalogErrorKind::IndexOverflow:
            return "index_overflow";
        case FontCatalogErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
    }
    return "invalid";
}

bool build_font_catalog(
    std::span<const FontFaceSeed> seeds,
    FontCatalog* output,
    FontCatalogStats* stats,
    FontCatalogError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    clear_error(error);
    output->release();
    *stats = {};

    if (seeds.size() > static_cast<std::size_t>(
                           std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            FontCatalogErrorKind::IndexOverflow,
            0U,
            "font face count exceeds the 32-bit catalog contract",
            error);
    }

    try {
        std::pmr::vector<std::size_t> order(output->resource());
        order.reserve(seeds.size());
        for (std::size_t index = 0U; index < seeds.size(); ++index) {
            if (!validate_seed(seeds[index], index, error)) {
                return false;
            }
            order.push_back(index);
        }
        std::sort(
            order.begin(),
            order.end(),
            [&seeds](std::size_t left, std::size_t right) {
                return seeds[left].stable_key < seeds[right].stable_key;
            });

        std::size_t canonical_ranges = 0U;
        std::uint64_t input_ranges = 0U;
        for (std::size_t position = 0U; position < order.size(); ++position) {
            const FontFaceSeed& seed = seeds[order[position]];
            if (position != 0U &&
                seeds[order[position - 1U]].stable_key == seed.stable_key) {
                return fail(
                    FontCatalogErrorKind::DuplicateStableKey,
                    order[position],
                    "font stable keys must be unique",
                    error);
            }
            input_ranges += static_cast<std::uint64_t>(seed.coverage.size());
            const std::size_t face_ranges = canonical_range_count(seed.coverage);
            if (face_ranges > std::numeric_limits<std::size_t>::max() - canonical_ranges) {
                return fail(
                    FontCatalogErrorKind::IndexOverflow,
                    order[position],
                    "font coverage range count overflowed",
                    error);
            }
            canonical_ranges += face_ranges;
        }
        if (canonical_ranges > static_cast<std::size_t>(
                                   std::numeric_limits<std::uint32_t>::max())) {
            return fail(
                FontCatalogErrorKind::IndexOverflow,
                0U,
                "canonical font coverage exceeds the 32-bit catalog contract",
                error);
        }

        FontCatalog working(output->resource());
        working.faces.reserve(seeds.size());
        working.coverage_ranges.reserve(canonical_ranges);

        FontCatalogStats working_stats;
        working_stats.input_faces = static_cast<std::uint64_t>(seeds.size());
        working_stats.input_coverage_ranges = input_ranges;

        for (const std::size_t source_index : order) {
            const FontFaceSeed& seed = seeds[source_index];
            const std::size_t coverage_offset = working.coverage_ranges.size();
            for (const FontCoverageRange range : seed.coverage) {
                if (working.coverage_ranges.size() != coverage_offset) {
                    FontCoverageRange& previous = working.coverage_ranges.back();
                    if (previous.last != kMaximumUnicodeCodePoint &&
                        range.first == previous.last + 1U) {
                        previous.last = range.last;
                        ++working_stats.adjacent_ranges_merged;
                        continue;
                    }
                }
                working.coverage_ranges.push_back(range);
            }
            const std::size_t coverage_count =
                working.coverage_ranges.size() - coverage_offset;
            working.faces.push_back(FontFaceRecord{
                seed.stable_key,
                seed.family_key,
                static_cast<std::uint32_t>(coverage_offset),
                static_cast<std::uint32_t>(coverage_count),
                seed.weight,
                seed.preferred_script,
                seed.width,
                seed.slant,
                seed.flags});
            if ((seed.flags & kFontFaceVariable) != 0U) {
                ++working_stats.variable_faces;
            }
            if ((seed.flags & kFontFaceColor) != 0U) {
                ++working_stats.color_faces;
            }
        }

        working_stats.output_faces =
            static_cast<std::uint64_t>(working.faces.size());
        working_stats.output_coverage_ranges =
            static_cast<std::uint64_t>(working.coverage_ranges.size());
        output->faces.swap(working.faces);
        output->coverage_ranges.swap(working.coverage_ranges);
        *stats = working_stats;
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            FontCatalogErrorKind::OutputBudgetExceeded,
            0U,
            "font catalog exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            FontCatalogErrorKind::OutputBudgetExceeded,
            0U,
            "font catalog allocation failed",
            error);
    }
}

FontFaceId font_face_id_by_stable_key(
    const FontCatalog& catalog,
    std::uint64_t stable_key) noexcept {
    const auto iterator = std::lower_bound(
        catalog.faces.begin(),
        catalog.faces.end(),
        stable_key,
        [](const FontFaceRecord& face, std::uint64_t value) {
            return face.stable_key < value;
        });
    if (iterator == catalog.faces.end() || iterator->stable_key != stable_key) {
        return kInvalidFontFaceId;
    }
    return static_cast<FontFaceId>(iterator - catalog.faces.begin());
}

bool font_face_covers(
    const FontCatalog& catalog,
    FontFaceId face_id,
    std::uint32_t codepoint) noexcept {
    if (face_id >= catalog.faces.size() || codepoint > kMaximumUnicodeCodePoint) {
        return false;
    }
    const FontFaceRecord& face = catalog.faces[face_id];
    const std::size_t first = face.coverage_offset;
    const std::size_t count = face.coverage_count;
    if (first > catalog.coverage_ranges.size() ||
        count > catalog.coverage_ranges.size() - first) {
        return false;
    }
    const auto begin = catalog.coverage_ranges.begin() +
                       static_cast<std::ptrdiff_t>(first);
    const auto end = begin + static_cast<std::ptrdiff_t>(count);
    const auto iterator = std::lower_bound(
        begin,
        end,
        codepoint,
        [](const FontCoverageRange& range, std::uint32_t value) {
            return range.last < value;
        });
    return iterator != end && codepoint >= iterator->first;
}

} // namespace zevryon::text
