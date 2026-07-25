#include "line_box_layout.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

void clear_error(LineBoxLayoutError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    LineBoxLayoutErrorKind kind,
    std::size_t line_index,
    std::size_t fragment_index,
    std::uint32_t segment_index,
    FontFaceId face_id,
    const char* message,
    LineBoxLayoutError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->line_index = line_index;
        error->fragment_index = fragment_index;
        error->segment_index = segment_index;
        error->face_id = face_id;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool checked_add(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr ||
        right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *output = left + right;
    return true;
}

bool valid_metric_record(const FontLineMetricRecord& record) noexcept {
    if (record.face_id == kInvalidFontFaceId ||
        record.units_per_em < 16U || record.units_per_em > 16'384U ||
        record.ascender < 0 || record.descender > 0 ||
        static_cast<std::uint8_t>(record.source) >
            static_cast<std::uint8_t>(
                FontLineMetricSource::Os2TypographicFallback)) {
        return false;
    }
    const std::int64_t height =
        static_cast<std::int64_t>(record.ascender) -
        static_cast<std::int64_t>(record.descender) +
        static_cast<std::int64_t>(record.line_gap);
    return height > 0;
}

bool validate_metric_table(
    const FontLineMetricTable& table,
    LineBoxLayoutError* error) noexcept {
    if (table.records.empty()) {
        return fail(
            LineBoxLayoutErrorKind::InvalidMetricTable,
            0U,
            0U,
            0U,
            kInvalidFontFaceId,
            "line-box layout requires a non-empty font metric table",
            error);
    }
    FontFaceId previous = kInvalidFontFaceId;
    bool have_previous = false;
    for (std::size_t index = 0U; index < table.records.size(); ++index) {
        const FontLineMetricRecord& record = table.records[index];
        if (!valid_metric_record(record) ||
            (have_previous && record.face_id <= previous)) {
            return fail(
                LineBoxLayoutErrorKind::InvalidMetricTable,
                0U,
                index,
                0U,
                record.face_id,
                "font metric table is invalid or not strictly ordered",
                error);
        }
        previous = record.face_id;
        have_previous = true;
    }
    return true;
}

bool scale_design_metric(
    std::int32_t design_value,
    std::int32_t y_scale,
    std::uint32_t units_per_em,
    std::int64_t* output) noexcept {
    if (output == nullptr || y_scale <= 0 || units_per_em == 0U) {
        return false;
    }
    const std::int64_t value = design_value;
    const bool negative = value < 0;
    const std::uint64_t magnitude = static_cast<std::uint64_t>(
        negative ? -value : value);
    const std::uint64_t scale = static_cast<std::uint32_t>(y_scale);
    if (magnitude != 0U &&
        scale > std::numeric_limits<std::uint64_t>::max() / magnitude) {
        return false;
    }
    const std::uint64_t product = magnitude * scale;
    const std::uint64_t half = units_per_em / 2U;
    if (product > std::numeric_limits<std::uint64_t>::max() - half) {
        return false;
    }
    const std::uint64_t rounded = (product + half) / units_per_em;
    if (rounded > static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    const std::int64_t signed_value = static_cast<std::int64_t>(rounded);
    *output = negative ? -signed_value : signed_value;
    return true;
}

struct ScaledLineMetrics final {
    std::uint64_t baseline_offset{0};
    std::uint64_t block_size{0};
    bool negative_line_gap{false};
};

bool scale_line_metrics(
    const FontLineMetricRecord& record,
    std::int32_t y_scale,
    ScaledLineMetrics* output) noexcept {
    if (output == nullptr || !valid_metric_record(record) || y_scale <= 0) {
        return false;
    }
    std::int64_t ascender = 0;
    std::int64_t descender = 0;
    std::int64_t line_gap = 0;
    if (!scale_design_metric(
            record.ascender,
            y_scale,
            record.units_per_em,
            &ascender) ||
        !scale_design_metric(
            record.descender,
            y_scale,
            record.units_per_em,
            &descender) ||
        !scale_design_metric(
            record.line_gap,
            y_scale,
            record.units_per_em,
            &line_gap)) {
        return false;
    }
    if (ascender < 0 || descender > 0) {
        return false;
    }

    // C++ division truncates toward zero. Assign the odd signed remainder to the
    // block-after side so before + after is always exactly line_gap.
    const std::int64_t leading_before = line_gap / 2;
    const std::int64_t leading_after = line_gap - leading_before;
    const std::int64_t effective_ascent = ascender + leading_before;
    const std::int64_t effective_descent = -descender + leading_after;
    if (effective_ascent < 0 || effective_descent < 0) {
        return false;
    }
    const std::uint64_t ascent = static_cast<std::uint64_t>(effective_ascent);
    const std::uint64_t descent = static_cast<std::uint64_t>(effective_descent);
    std::uint64_t block_size = 0U;
    if (!checked_add(ascent, descent, &block_size) || block_size == 0U) {
        return false;
    }
    *output = ScaledLineMetrics{ascent, block_size, line_gap < 0};
    return true;
}

std::uint32_t metric_source_flags(
    const FontLineMetricRecord& record) noexcept {
    std::uint32_t flags = 0U;
    if (record.source == FontLineMetricSource::HorizontalHeader) {
        flags |= kFragmentBlockMetricHhea;
    } else {
        flags |= kFragmentBlockMetricOs2Typographic;
    }
    if ((record.flags & kFontLineMetricNegativeLineGap) != 0U) {
        flags |= kFragmentBlockMetricNegativeLineGap;
    }
    return flags;
}

bool horizontal_direction(ShapingDirection direction) noexcept {
    return direction == ShapingDirection::LeftToRight ||
           direction == ShapingDirection::RightToLeft;
}

} // namespace

LineBoxLayout::LineBoxLayout(std::pmr::memory_resource* resource)
    : lines(resource), fragment_metrics(resource) {}

std::pmr::memory_resource* LineBoxLayout::resource() const noexcept {
    return lines.get_allocator().resource();
}

void LineBoxLayout::release() noexcept {
    release_vector(&lines);
    release_vector(&fragment_metrics);
}

const char* line_box_layout_error_kind_name(
    LineBoxLayoutErrorKind kind) noexcept {
    switch (kind) {
        case LineBoxLayoutErrorKind::None:
            return "none";
        case LineBoxLayoutErrorKind::InvalidInput:
            return "invalid_input";
        case LineBoxLayoutErrorKind::InvalidMetricTable:
            return "invalid_metric_table";
        case LineBoxLayoutErrorKind::TopologyViolation:
            return "topology_violation";
        case LineBoxLayoutErrorKind::MissingFaceMetrics:
            return "missing_face_metrics";
        case LineBoxLayoutErrorKind::InvalidScale:
            return "invalid_scale";
        case LineBoxLayoutErrorKind::MetricOverflow:
            return "metric_overflow";
        case LineBoxLayoutErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
        case LineBoxLayoutErrorKind::AggregateOverflow:
            return "aggregate_overflow";
    }
    return "invalid";
}

bool build_line_box_layout(
    const LineBoxLayoutRequest& request,
    LineBoxLayout* output,
    LineBoxLayoutStats* stats,
    LineBoxLayoutError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);

    if (request.fragment_layout == nullptr || request.shaped_text == nullptr ||
        request.font_metrics == nullptr ||
        request.strut_face_id == kInvalidFontFaceId ||
        request.strut_y_scale <= 0) {
        return fail(
            LineBoxLayoutErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            request.strut_face_id,
            "line-box layout requires fragment, shaping, metric and strut inputs",
            error);
    }
    if (!validate_metric_table(*request.font_metrics, error)) {
        return false;
    }
    const FontLineMetricRecord* strut_record = find_font_line_metric(
        *request.font_metrics,
        request.strut_face_id);
    if (strut_record == nullptr) {
        return fail(
            LineBoxLayoutErrorKind::MissingFaceMetrics,
            0U,
            0U,
            0U,
            request.strut_face_id,
            "strut face has no font-line metric record",
            error);
    }
    ScaledLineMetrics strut;
    if (!scale_line_metrics(*strut_record, request.strut_y_scale, &strut)) {
        return fail(
            LineBoxLayoutErrorKind::InvalidScale,
            0U,
            0U,
            0U,
            request.strut_face_id,
            "strut metrics cannot be represented at the requested y scale",
            error);
    }

    const LineFragmentLayout& source = *request.fragment_layout;
    const MultiRunShapedText& shaped = *request.shaped_text;
    LineBoxLayoutStats working_stats;
    working_stats.input_lines = source.lines.size();
    working_stats.input_fragments = source.fragments.size();
    working_stats.input_segments = shaped.segments.size();
    working_stats.input_metric_records = request.font_metrics->records.size();

    try {
        LineBoxLayout working(output->resource());
        working.lines.reserve(source.lines.size());
        working.fragment_metrics.reserve(source.fragments.size());

        std::size_t expected_first_fragment = 0U;
        std::uint32_t previous_cluster_limit = 0U;
        std::uint64_t block_cursor = 0U;
        for (std::size_t line_index = 0U;
             line_index < source.lines.size();
             ++line_index) {
            const VisualLineLayoutRecord& line = source.lines[line_index];
            if (line.first_fragment != expected_first_fragment ||
                line.fragment_count >
                    source.fragments.size() - expected_first_fragment ||
                (line_index != 0U &&
                 line.cluster_limit < previous_cluster_limit)) {
                return fail(
                    LineBoxLayoutErrorKind::TopologyViolation,
                    line_index,
                    expected_first_fragment,
                    0U,
                    kInvalidFontFaceId,
                    "visual line fragment slices are not a contiguous logical partition",
                    error);
            }

            const std::size_t first_metric = working.fragment_metrics.size();
            std::uint64_t line_ascent = strut.baseline_offset;
            std::uint64_t line_descent =
                strut.block_size - strut.baseline_offset;
            bool expanded = false;
            bool mixed_metrics = false;
            bool contains_negative_gap = strut.negative_line_gap;

            for (std::size_t relative = 0U;
                 relative < line.fragment_count;
                 ++relative) {
                const std::size_t fragment_index =
                    expected_first_fragment + relative;
                const InlineLayoutFragment& fragment =
                    source.fragments[fragment_index];
                if (fragment.segment_index >= shaped.segments.size() ||
                    fragment.inline_offset > line.inline_advance ||
                    fragment.inline_advance >
                        line.inline_advance - fragment.inline_offset ||
                    fragment.first_cluster >= fragment.cluster_limit) {
                    return fail(
                        LineBoxLayoutErrorKind::TopologyViolation,
                        line_index,
                        fragment_index,
                        fragment.segment_index,
                        kInvalidFontFaceId,
                        "visual fragment is outside its line or shaped segment domain",
                        error);
                }
                const MultiRunShapedSegment& segment =
                    shaped.segments[fragment.segment_index];
                if (!horizontal_direction(segment.run.direction) ||
                    segment.run.face_id == kInvalidFontFaceId ||
                    fragment.first_cluster < segment.run.cluster_index ||
                    fragment.cluster_limit > segment.glyphs.cluster_limit ||
                    segment.glyphs.first_cluster != segment.run.cluster_index ||
                    segment.glyphs.cluster_limit <=
                        segment.glyphs.first_cluster) {
                    return fail(
                        LineBoxLayoutErrorKind::TopologyViolation,
                        line_index,
                        fragment_index,
                        fragment.segment_index,
                        segment.run.face_id,
                        "shaped segment topology is inconsistent with its visual fragment",
                        error);
                }
                if (segment.glyphs.y_scale <= 0) {
                    return fail(
                        LineBoxLayoutErrorKind::InvalidScale,
                        line_index,
                        fragment_index,
                        fragment.segment_index,
                        segment.run.face_id,
                        "shaped segment does not retain a positive HarfBuzz y scale",
                        error);
                }
                const FontLineMetricRecord* record = find_font_line_metric(
                    *request.font_metrics,
                    segment.run.face_id);
                if (record == nullptr) {
                    return fail(
                        LineBoxLayoutErrorKind::MissingFaceMetrics,
                        line_index,
                        fragment_index,
                        fragment.segment_index,
                        segment.run.face_id,
                        "visual fragment face has no font-line metric record",
                        error);
                }
                ScaledLineMetrics scaled;
                if (!scale_line_metrics(
                        *record,
                        segment.glyphs.y_scale,
                        &scaled)) {
                    return fail(
                        LineBoxLayoutErrorKind::MetricOverflow,
                        line_index,
                        fragment_index,
                        fragment.segment_index,
                        segment.run.face_id,
                        "font line metrics cannot be represented at the shaped y scale",
                        error);
                }
                const std::uint64_t descent =
                    scaled.block_size - scaled.baseline_offset;
                line_ascent = std::max(line_ascent, scaled.baseline_offset);
                line_descent = std::max(line_descent, descent);
                contains_negative_gap =
                    contains_negative_gap || scaled.negative_line_gap;
                mixed_metrics = mixed_metrics ||
                    segment.run.face_id != request.strut_face_id ||
                    segment.glyphs.y_scale != request.strut_y_scale;

                std::uint32_t flags = metric_source_flags(*record);
                if (segment.run.face_id == request.strut_face_id &&
                    segment.glyphs.y_scale == request.strut_y_scale) {
                    flags |= kFragmentBlockMetricMatchesStrut;
                }
                working.fragment_metrics.push_back(FragmentBlockMetric{
                    0U,
                    scaled.block_size,
                    scaled.baseline_offset,
                    flags,
                    0U});
                if (record->source == FontLineMetricSource::HorizontalHeader) {
                    ++working_stats.hhea_fragment_metrics;
                } else {
                    ++working_stats.os2_fragment_metrics;
                }
                if (scaled.negative_line_gap) {
                    ++working_stats.negative_gap_fragment_metrics;
                }
                working_stats.maximum_fragment_block_size = std::max(
                    working_stats.maximum_fragment_block_size,
                    scaled.block_size);
            }

            std::uint64_t line_block_size = 0U;
            if (!checked_add(line_ascent, line_descent, &line_block_size)) {
                return fail(
                    LineBoxLayoutErrorKind::AggregateOverflow,
                    line_index,
                    expected_first_fragment,
                    0U,
                    kInvalidFontFaceId,
                    "line ascent and descent overflow the block-size contract",
                    error);
            }
            expanded = line_ascent > strut.baseline_offset ||
                line_descent >
                    strut.block_size - strut.baseline_offset;
            const std::size_t metric_limit = working.fragment_metrics.size();
            for (std::size_t metric_index = first_metric;
                 metric_index < metric_limit;
                 ++metric_index) {
                FragmentBlockMetric& metric =
                    working.fragment_metrics[metric_index];
                metric.block_offset = line_ascent - metric.baseline_offset;
            }

            std::uint64_t baseline = 0U;
            std::uint64_t next_block_cursor = 0U;
            if (!checked_add(block_cursor, line_ascent, &baseline) ||
                !checked_add(
                    block_cursor,
                    line_block_size,
                    &next_block_cursor)) {
                return fail(
                    LineBoxLayoutErrorKind::AggregateOverflow,
                    line_index,
                    expected_first_fragment,
                    0U,
                    kInvalidFontFaceId,
                    "line block positions overflow the 64-bit layout contract",
                    error);
            }

            std::uint32_t flags = line.flags;
            if (expanded) {
                flags |= kLineBoxExpandedBeyondStrut;
                ++working_stats.expanded_lines;
            }
            if (mixed_metrics) {
                flags |= kLineBoxContainsMixedMetrics;
                ++working_stats.mixed_metric_lines;
            }
            if (contains_negative_gap) {
                flags |= kLineBoxContainsNegativeLineGap;
            }
            if (line.fragment_count == 0U) {
                ++working_stats.empty_lines;
            }
            if (first_metric >
                    static_cast<std::size_t>(
                        std::numeric_limits<std::uint32_t>::max()) ||
                line.fragment_count >
                    static_cast<std::size_t>(
                        std::numeric_limits<std::uint32_t>::max())) {
                return fail(
                    LineBoxLayoutErrorKind::AggregateOverflow,
                    line_index,
                    expected_first_fragment,
                    0U,
                    kInvalidFontFaceId,
                    "line fragment-metric slice exceeds the 32-bit contract",
                    error);
            }
            working.lines.push_back(LineBoxRecord{
                block_cursor,
                line_block_size,
                baseline,
                line.inline_advance,
                static_cast<std::uint32_t>(first_metric),
                line.fragment_count,
                line.cluster_limit,
                flags});

            working_stats.maximum_line_block_size = std::max(
                working_stats.maximum_line_block_size,
                line_block_size);
            working_stats.maximum_line_ascent = std::max(
                working_stats.maximum_line_ascent,
                line_ascent);
            working_stats.maximum_line_descent = std::max(
                working_stats.maximum_line_descent,
                line_descent);
            block_cursor = next_block_cursor;
            expected_first_fragment += line.fragment_count;
            previous_cluster_limit = line.cluster_limit;
        }

        if (expected_first_fragment != source.fragments.size() ||
            working.fragment_metrics.size() != source.fragments.size()) {
            return fail(
                LineBoxLayoutErrorKind::TopologyViolation,
                source.lines.size(),
                expected_first_fragment,
                0U,
                kInvalidFontFaceId,
                "visual fragment array is not fully owned by the line partition",
                error);
        }
        working_stats.output_lines = working.lines.size();
        working_stats.output_fragment_metrics =
            working.fragment_metrics.size();
        working_stats.total_block_extent = block_cursor;
        output->lines.swap(working.lines);
        output->fragment_metrics.swap(working.fragment_metrics);
        *stats = working_stats;
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            LineBoxLayoutErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            kInvalidFontFaceId,
            "line-box layout exceeded its resource budget",
            error);
    } catch (...) {
        return fail(
            LineBoxLayoutErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            kInvalidFontFaceId,
            "line-box layout allocation failed",
            error);
    }
}

} // namespace zevryon::text
