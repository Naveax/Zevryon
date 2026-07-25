#include "font_line_metrics.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::uint32_t kHeadTag = sfnt_tag('h', 'e', 'a', 'd');
constexpr std::uint32_t kHheaTag = sfnt_tag('h', 'h', 'e', 'a');
constexpr std::uint32_t kOs2Tag = sfnt_tag('O', 'S', '/', '2');
constexpr std::uint32_t kHheaVersion = 0x00010000U;
constexpr std::uint16_t kUseTypoMetricsMask = 1U << 7U;
constexpr std::size_t kHeadMinimumBytes = 54U;
constexpr std::size_t kHheaMinimumBytes = 36U;
constexpr std::size_t kOs2MinimumBytes = 78U;

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

void clear_error(FontLineMetricError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    FontLineMetricErrorKind kind,
    std::size_t binding_index,
    FontFaceId face_id,
    std::uint32_t table_tag,
    std::size_t byte_offset,
    const char* message,
    FontLineMetricError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->binding_index = binding_index;
        error->face_id = face_id;
        error->table_tag = table_tag;
        error->byte_offset = byte_offset;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool read_u16(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::uint16_t* output) noexcept {
    if (output == nullptr || offset > bytes.size() || bytes.size() - offset < 2U) {
        return false;
    }
    const auto first = static_cast<std::uint16_t>(
        std::to_integer<unsigned char>(bytes[offset]));
    const auto second = static_cast<std::uint16_t>(
        std::to_integer<unsigned char>(bytes[offset + 1U]));
    *output = static_cast<std::uint16_t>((first << 8U) | second);
    return true;
}

bool read_s16(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::int32_t* output) noexcept {
    std::uint16_t raw = 0U;
    if (output == nullptr || !read_u16(bytes, offset, &raw)) {
        return false;
    }
    const std::int16_t signed_value = static_cast<std::int16_t>(raw);
    *output = signed_value;
    return true;
}

bool read_u32(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::uint32_t* output) noexcept {
    if (output == nullptr || offset > bytes.size() || bytes.size() - offset < 4U) {
        return false;
    }
    *output =
        (static_cast<std::uint32_t>(
             std::to_integer<unsigned char>(bytes[offset])) << 24U) |
        (static_cast<std::uint32_t>(
             std::to_integer<unsigned char>(bytes[offset + 1U])) << 16U) |
        (static_cast<std::uint32_t>(
             std::to_integer<unsigned char>(bytes[offset + 2U])) << 8U) |
        static_cast<std::uint32_t>(
            std::to_integer<unsigned char>(bytes[offset + 3U]));
    return true;
}

bool valid_metric_triplet(
    std::int32_t ascender,
    std::int32_t descender,
    std::int32_t line_gap,
    std::uint64_t* line_height) noexcept {
    const std::int64_t height =
        static_cast<std::int64_t>(ascender) -
        static_cast<std::int64_t>(descender) +
        static_cast<std::int64_t>(line_gap);
    if (ascender < 0 || descender > 0 || height <= 0) {
        return false;
    }
    if (line_height != nullptr) {
        *line_height = static_cast<std::uint64_t>(height);
    }
    return true;
}

struct ParsedTriplet final {
    std::int32_t ascender{0};
    std::int32_t descender{0};
    std::int32_t line_gap{0};
    bool present{false};
    bool valid{false};
};

bool parse_hhea(
    const SfntResourceView& view,
    FontFaceId face_id,
    ParsedTriplet* output,
    FontLineMetricError* error) noexcept {
    SfntTableRecord record;
    if (!view.find_table(kHheaTag, &record)) {
        *output = {};
        return true;
    }
    const std::span<const std::byte> bytes = view.table_data(record);
    if (bytes.size() < kHheaMinimumBytes) {
        return fail(
            FontLineMetricErrorKind::InvalidHheaTable,
            0U,
            face_id,
            kHheaTag,
            record.offset,
            "hhea table is shorter than the required horizontal-header prefix",
            error);
    }
    std::uint32_t version = 0U;
    ParsedTriplet parsed;
    parsed.present = true;
    if (!read_u32(bytes, 0U, &version) ||
        !read_s16(bytes, 4U, &parsed.ascender) ||
        !read_s16(bytes, 6U, &parsed.descender) ||
        !read_s16(bytes, 8U, &parsed.line_gap)) {
        return fail(
            FontLineMetricErrorKind::InvalidHheaTable,
            0U,
            face_id,
            kHheaTag,
            record.offset,
            "hhea metrics are truncated",
            error);
    }
    if (version != kHheaVersion) {
        return fail(
            FontLineMetricErrorKind::InvalidHheaTable,
            0U,
            face_id,
            kHheaTag,
            record.offset,
            "hhea table version is not 1.0",
            error);
    }
    parsed.valid = valid_metric_triplet(
        parsed.ascender,
        parsed.descender,
        parsed.line_gap,
        nullptr);
    *output = parsed;
    return true;
}

struct ParsedOs2 final {
    ParsedTriplet typo;
    std::uint32_t win_ascent{0};
    std::uint32_t win_descent{0};
    bool present{false};
    bool use_typo_metrics{false};
};

bool parse_os2(
    const SfntResourceView& view,
    FontFaceId face_id,
    ParsedOs2* output,
    FontLineMetricError* error) noexcept {
    SfntTableRecord record;
    if (!view.find_table(kOs2Tag, &record)) {
        *output = {};
        return true;
    }
    const std::span<const std::byte> bytes = view.table_data(record);
    if (bytes.size() < kOs2MinimumBytes) {
        return fail(
            FontLineMetricErrorKind::InvalidOs2Table,
            0U,
            face_id,
            kOs2Tag,
            record.offset,
            "OS/2 table is shorter than the version-zero metric prefix",
            error);
    }
    std::uint16_t fs_selection = 0U;
    std::uint16_t win_ascent = 0U;
    std::uint16_t win_descent = 0U;
    ParsedOs2 parsed;
    parsed.present = true;
    parsed.typo.present = true;
    if (!read_u16(bytes, 62U, &fs_selection) ||
        !read_s16(bytes, 68U, &parsed.typo.ascender) ||
        !read_s16(bytes, 70U, &parsed.typo.descender) ||
        !read_s16(bytes, 72U, &parsed.typo.line_gap) ||
        !read_u16(bytes, 74U, &win_ascent) ||
        !read_u16(bytes, 76U, &win_descent)) {
        return fail(
            FontLineMetricErrorKind::InvalidOs2Table,
            0U,
            face_id,
            kOs2Tag,
            record.offset,
            "OS/2 line metrics are truncated",
            error);
    }
    parsed.typo.valid = valid_metric_triplet(
        parsed.typo.ascender,
        parsed.typo.descender,
        parsed.typo.line_gap,
        nullptr);
    parsed.win_ascent = win_ascent;
    parsed.win_descent = win_descent;
    parsed.use_typo_metrics = (fs_selection & kUseTypoMetricsMask) != 0U;
    *output = parsed;
    return true;
}

} // namespace

FontLineMetricTable::FontLineMetricTable(std::pmr::memory_resource* resource)
    : records(resource) {}

std::pmr::memory_resource* FontLineMetricTable::resource() const noexcept {
    return records.get_allocator().resource();
}

void FontLineMetricTable::release() noexcept {
    release_vector(&records);
}

const char* font_line_metric_source_name(FontLineMetricSource source) noexcept {
    switch (source) {
        case FontLineMetricSource::Os2Typographic:
            return "os2_typographic";
        case FontLineMetricSource::HorizontalHeader:
            return "horizontal_header";
        case FontLineMetricSource::Os2TypographicFallback:
            return "os2_typographic_fallback";
    }
    return "invalid";
}

const char* font_line_metric_error_kind_name(
    FontLineMetricErrorKind kind) noexcept {
    switch (kind) {
        case FontLineMetricErrorKind::None:
            return "none";
        case FontLineMetricErrorKind::InvalidArgument:
            return "invalid_argument";
        case FontLineMetricErrorKind::InvalidBindingTable:
            return "invalid_binding_table";
        case FontLineMetricErrorKind::GenerationMismatch:
            return "generation_mismatch";
        case FontLineMetricErrorKind::MissingHeadTable:
            return "missing_head_table";
        case FontLineMetricErrorKind::InvalidHeadTable:
            return "invalid_head_table";
        case FontLineMetricErrorKind::InvalidOs2Table:
            return "invalid_os2_table";
        case FontLineMetricErrorKind::InvalidHheaTable:
            return "invalid_hhea_table";
        case FontLineMetricErrorKind::InvalidMetrics:
            return "invalid_metrics";
        case FontLineMetricErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
        case FontLineMetricErrorKind::AggregateOverflow:
            return "aggregate_overflow";
    }
    return "invalid";
}

bool read_sfnt_font_line_metric_record(
    FontFaceId face_id,
    const SfntResourceView& view,
    FontLineMetricRecord* output,
    FontLineMetricError* error) noexcept {
    if (output != nullptr) {
        *output = {};
    }
    clear_error(error);
    if (output == nullptr || face_id == kInvalidFontFaceId || !view.valid()) {
        return fail(
            FontLineMetricErrorKind::InvalidArgument,
            0U,
            face_id,
            0U,
            0U,
            "font-line metrics require a valid face and sfnt resource view",
            error);
    }

    SfntTableRecord head_record;
    if (!view.find_table(kHeadTag, &head_record)) {
        return fail(
            FontLineMetricErrorKind::MissingHeadTable,
            0U,
            face_id,
            kHeadTag,
            0U,
            "font face does not contain a head table",
            error);
    }
    const std::span<const std::byte> head = view.table_data(head_record);
    std::uint16_t units_per_em = 0U;
    if (head.size() < kHeadMinimumBytes ||
        !read_u16(head, 18U, &units_per_em) ||
        units_per_em < 16U || units_per_em > 16'384U) {
        return fail(
            FontLineMetricErrorKind::InvalidHeadTable,
            0U,
            face_id,
            kHeadTag,
            static_cast<std::size_t>(head_record.offset) + 18U,
            "head unitsPerEm is truncated or outside the OpenType range",
            error);
    }

    ParsedTriplet hhea;
    if (!parse_hhea(view, face_id, &hhea, error)) {
        return false;
    }
    ParsedOs2 os2;
    if (!parse_os2(view, face_id, &os2, error)) {
        return false;
    }

    FontLineMetricRecord record;
    record.face_id = face_id;
    record.units_per_em = units_per_em;
    if (hhea.present) {
        record.flags |= kFontLineMetricHasHhea;
    }
    if (os2.present) {
        record.flags |= kFontLineMetricHasOs2;
        record.win_ascent = os2.win_ascent;
        record.win_descent = os2.win_descent;
    }
    if (os2.use_typo_metrics) {
        record.flags |= kFontLineMetricUseTypoMetrics;
        if (!os2.typo.valid) {
            return fail(
                FontLineMetricErrorKind::InvalidMetrics,
                0U,
                face_id,
                kOs2Tag,
                68U,
                "USE_TYPO_METRICS selects an invalid OS/2 sTypo metric triple",
                error);
        }
        record.ascender = os2.typo.ascender;
        record.descender = os2.typo.descender;
        record.line_gap = os2.typo.line_gap;
        record.source = FontLineMetricSource::Os2Typographic;
    } else if (hhea.valid) {
        record.ascender = hhea.ascender;
        record.descender = hhea.descender;
        record.line_gap = hhea.line_gap;
        record.source = FontLineMetricSource::HorizontalHeader;
    } else if (os2.typo.valid) {
        record.ascender = os2.typo.ascender;
        record.descender = os2.typo.descender;
        record.line_gap = os2.typo.line_gap;
        record.source = FontLineMetricSource::Os2TypographicFallback;
    } else {
        return fail(
            FontLineMetricErrorKind::InvalidMetrics,
            0U,
            face_id,
            0U,
            0U,
            "font face has no valid horizontal line-metric triple",
            error);
    }
    if (record.line_gap < 0) {
        record.flags |= kFontLineMetricNegativeLineGap;
    }
    *output = record;
    return true;
}

bool read_font_line_metric_record(
    FontFaceId face_id,
    const VerifiedFontResource& resource,
    FontLineMetricRecord* output,
    FontLineMetricError* error) noexcept {
    return read_sfnt_font_line_metric_record(
        face_id, resource.view(), output, error);
}

bool build_font_line_metric_table(
    std::span<const CatalogFontFaceBinding> bindings,
    FontLineMetricTable* output,
    FontLineMetricStats* stats,
    FontLineMetricError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);
    stats->input_bindings = bindings.size();
    if (bindings.empty()) {
        return fail(
            FontLineMetricErrorKind::InvalidArgument,
            0U,
            kInvalidFontFaceId,
            0U,
            0U,
            "font-line metric table requires at least one binding",
            error);
    }

    try {
        FontLineMetricTable working(output->resource());
        working.records.reserve(bindings.size());
        std::uint64_t generation_id = 0U;
        FontFaceId previous_face = kInvalidFontFaceId;
        bool have_previous = false;
        for (std::size_t index = 0U; index < bindings.size(); ++index) {
            const CatalogFontFaceBinding& binding = bindings[index];
            if (!binding.valid() || binding.resource() == nullptr ||
                binding.face_id() == kInvalidFontFaceId) {
                return fail(
                    FontLineMetricErrorKind::InvalidBindingTable,
                    index,
                    binding.face_id(),
                    0U,
                    0U,
                    "font-line metric binding is invalid or has no resource",
                    error);
            }
            if (have_previous && binding.face_id() <= previous_face) {
                return fail(
                    FontLineMetricErrorKind::InvalidBindingTable,
                    index,
                    binding.face_id(),
                    0U,
                    0U,
                    "font-line metric bindings are not strictly ordered by face_id",
                    error);
            }
            if (index == 0U) {
                generation_id = binding.generation_id();
                if (generation_id == 0U) {
                    return fail(
                        FontLineMetricErrorKind::InvalidBindingTable,
                        index,
                        binding.face_id(),
                        0U,
                        0U,
                        "font-line metric binding has a zero generation id",
                        error);
                }
            } else if (binding.generation_id() != generation_id) {
                return fail(
                    FontLineMetricErrorKind::GenerationMismatch,
                    index,
                    binding.face_id(),
                    0U,
                    0U,
                    "font-line metric bindings span multiple catalog generations",
                    error);
            }

            FontLineMetricRecord record;
            FontLineMetricError record_error;
            if (!read_font_line_metric_record(
                    binding.face_id(),
                    *binding.resource(),
                    &record,
                    &record_error)) {
                *error = std::move(record_error);
                error->binding_index = index;
                return false;
            }
            working.records.push_back(record);
            std::uint64_t line_height = 0U;
            if (!valid_metric_triplet(
                    record.ascender,
                    record.descender,
                    record.line_gap,
                    &line_height)) {
                return fail(
                    FontLineMetricErrorKind::InvalidMetrics,
                    index,
                    binding.face_id(),
                    0U,
                    0U,
                    "selected font-line metric triple is invalid",
                    error);
            }
            stats->maximum_design_line_height = std::max(
                stats->maximum_design_line_height,
                line_height);
            stats->maximum_units_per_em = std::max(
                stats->maximum_units_per_em,
                record.units_per_em);
            switch (record.source) {
                case FontLineMetricSource::Os2Typographic:
                    ++stats->os2_typographic_records;
                    break;
                case FontLineMetricSource::HorizontalHeader:
                    ++stats->hhea_records;
                    break;
                case FontLineMetricSource::Os2TypographicFallback:
                    ++stats->os2_fallback_records;
                    break;
            }
            if ((record.flags & kFontLineMetricUseTypoMetrics) != 0U) {
                ++stats->use_typo_metrics_records;
            }
            if ((record.flags & kFontLineMetricNegativeLineGap) != 0U) {
                ++stats->negative_line_gap_records;
            }
            previous_face = binding.face_id();
            have_previous = true;
        }
        stats->generation_id = generation_id;
        stats->output_records = working.records.size();
        output->records.swap(working.records);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            FontLineMetricErrorKind::OutputBudgetExceeded,
            0U,
            kInvalidFontFaceId,
            0U,
            0U,
            "font-line metric table exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            FontLineMetricErrorKind::OutputBudgetExceeded,
            0U,
            kInvalidFontFaceId,
            0U,
            0U,
            "font-line metric table allocation failed",
            error);
    }
}

const FontLineMetricRecord* find_font_line_metric(
    const FontLineMetricTable& table,
    FontFaceId face_id) noexcept {
    const auto iterator = std::lower_bound(
        table.records.begin(),
        table.records.end(),
        face_id,
        [](const FontLineMetricRecord& record, FontFaceId value) {
            return record.face_id < value;
        });
    return iterator != table.records.end() && iterator->face_id == face_id
        ? &*iterator
        : nullptr;
}

} // namespace zevryon::text
