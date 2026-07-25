#include "line_break_opportunity.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::uint64_t kUnitClusterMask = 0xFFFFFFFFULL;
constexpr std::uint64_t kUnitClassShift = 32U;
constexpr std::uint64_t kUnitFlagsShift = 40U;
constexpr std::uint64_t kUnitMandatoryShift = 48U;

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

std::pmr::memory_resource* usable_resource(
    std::pmr::memory_resource* resource) noexcept {
    return resource != nullptr ? resource : std::pmr::get_default_resource();
}

void clear_error(LineBreakOpportunityError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool fail(
    LineBreakOpportunityErrorKind kind,
    std::size_t codepoint_index,
    std::size_t cluster_index,
    const char* message,
    LineBreakOpportunityError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->codepoint_index = codepoint_index;
        error->cluster_index = cluster_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool checked_add(std::uint64_t* value, std::uint64_t addition) noexcept {
    if (*value > std::numeric_limits<std::uint64_t>::max() - addition) {
        return false;
    }
    *value += addition;
    return true;
}

bool is_scalar(std::uint32_t value) noexcept {
    return value <= 0x10FFFFU &&
           !(value >= 0xD800U && value <= 0xDFFFU);
}

bool is_hard(LineBreakClass value) noexcept {
    return value == LineBreakClass::BK ||
           value == LineBreakClass::CR ||
           value == LineBreakClass::LF ||
           value == LineBreakClass::NL;
}

bool is_alphabetic(LineBreakClass value) noexcept {
    return value == LineBreakClass::AL || value == LineBreakClass::HL;
}

bool is_hangul(LineBreakClass value) noexcept {
    return value == LineBreakClass::JL ||
           value == LineBreakClass::JV ||
           value == LineBreakClass::JT ||
           value == LineBreakClass::H2 ||
           value == LineBreakClass::H3;
}

bool is_aksara_or_dotted(LineBreakClass value, std::uint8_t flags) noexcept {
    return value == LineBreakClass::AK ||
           (flags & kLineBreakDottedCircle) != 0U;
}

bool is_brahmic_base(LineBreakClass value, std::uint8_t flags) noexcept {
    return value == LineBreakClass::AS ||
           is_aksara_or_dotted(value, flags);
}

bool is_lb9_base(LineBreakClass value) noexcept {
    return value != LineBreakClass::BK &&
           value != LineBreakClass::CR &&
           value != LineBreakClass::LF &&
           value != LineBreakClass::NL &&
           value != LineBreakClass::SP &&
           value != LineBreakClass::ZW;
}

bool is_initial_quote(std::uint8_t flags) noexcept {
    return (flags & kLineBreakInitialQuote) != 0U;
}

bool is_final_quote(std::uint8_t flags) noexcept {
    return (flags & kLineBreakFinalQuote) != 0U;
}

bool is_east_asian(std::uint8_t flags) noexcept {
    return (flags & kLineBreakEastAsian) != 0U;
}

bool is_extended_pictographic_unassigned(std::uint8_t flags) noexcept {
    return (flags & kLineBreakExtendedPictographicUnassigned) != 0U;
}

std::uint64_t pack_unit(
    std::uint32_t cluster_index,
    LineBreakClass break_class,
    std::uint8_t flags,
    bool mandatory_after) noexcept {
    return static_cast<std::uint64_t>(cluster_index) |
           (static_cast<std::uint64_t>(
                static_cast<std::uint8_t>(break_class))
            << kUnitClassShift) |
           (static_cast<std::uint64_t>(flags) << kUnitFlagsShift) |
           (static_cast<std::uint64_t>(mandatory_after ? 1U : 0U)
            << kUnitMandatoryShift);
}

std::uint32_t unit_cluster(std::uint64_t unit) noexcept {
    return static_cast<std::uint32_t>(unit & kUnitClusterMask);
}

LineBreakClass unit_class(std::uint64_t unit) noexcept {
    return static_cast<LineBreakClass>(
        static_cast<std::uint8_t>(unit >> kUnitClassShift));
}

std::uint8_t unit_flags(std::uint64_t unit) noexcept {
    return static_cast<std::uint8_t>(unit >> kUnitFlagsShift);
}

bool unit_mandatory_after(std::uint64_t unit) noexcept {
    return ((unit >> kUnitMandatoryShift) & 1U) != 0U;
}

bool validate_codepoint_stream(
    std::span<const DecodedCodePoint> codepoints,
    LineBreakOpportunityStats* stats,
    LineBreakOpportunityError* error) noexcept {
    stats->input_codepoints = codepoints.size();
    for (std::size_t index = 0U; index < codepoints.size(); ++index) {
        const DecodedCodePoint& codepoint = codepoints[index];
        if (!is_scalar(codepoint.value) || codepoint.source_length == 0U) {
            return fail(
                LineBreakOpportunityErrorKind::InvalidCodePointStream,
                index,
                0U,
                "line breaking requires valid scalar values with non-empty source ranges",
                error);
        }
        if (index != 0U &&
            codepoint.source_start != codepoints[index - 1U].source_end()) {
            return fail(
                LineBreakOpportunityErrorKind::InvalidCodePointStream,
                index,
                0U,
                "decoded code-point source ranges must be contiguous",
                error);
        }
        if (codepoint.source_end() < codepoint.source_start) {
            return fail(
                LineBreakOpportunityErrorKind::InvalidCodePointStream,
                index,
                0U,
                "decoded code-point source range overflowed",
                error);
        }
    }
    return true;
}

bool validate_grapheme_topology(
    const LineBreakOpportunityRequest& request,
    std::size_t* cluster_count,
    LineBreakOpportunityStats* stats,
    LineBreakOpportunityError* error) noexcept {
    const auto codepoints = request.codepoints;
    const auto boundaries = request.grapheme_boundaries;
    if (codepoints.empty()) {
        if (!boundaries.empty()) {
            return fail(
                LineBreakOpportunityErrorKind::InvalidGraphemeTopology,
                0U,
                0U,
                "empty text requires an empty grapheme-boundary table",
                error);
        }
        *cluster_count = 0U;
        stats->input_clusters = 0U;
        return true;
    }
    if (boundaries.size() < 2U ||
        boundaries.front().codepoint_index != 0U ||
        boundaries.front().source_offset != codepoints.front().source_start ||
        boundaries.back().codepoint_index != codepoints.size() ||
        boundaries.back().source_offset != codepoints.back().source_end()) {
        return fail(
            LineBreakOpportunityErrorKind::InvalidGraphemeTopology,
            0U,
            0U,
            "grapheme boundaries must cover the complete decoded source domain",
            error);
    }
    const std::size_t count = boundaries.size() - 1U;
    if (count >= static_cast<std::size_t>(
                     std::numeric_limits<std::uint32_t>::max())) {
        return fail(
            LineBreakOpportunityErrorKind::ClusterDomainOverflow,
            0U,
            count,
            "line-break boundary indices exceed the compact uint32 domain",
            error);
    }
    for (std::size_t cluster = 0U; cluster < count; ++cluster) {
        const GraphemeBoundary& first = boundaries[cluster];
        const GraphemeBoundary& limit = boundaries[cluster + 1U];
        if (first.codepoint_index >= limit.codepoint_index ||
            limit.codepoint_index > codepoints.size() ||
            first.source_offset >= limit.source_offset) {
            return fail(
                LineBreakOpportunityErrorKind::InvalidGraphemeTopology,
                first.codepoint_index,
                cluster,
                "grapheme clusters must be non-empty and strictly ordered",
                error);
        }
        if (first.source_offset !=
                codepoints[first.codepoint_index].source_start ||
            limit.source_offset !=
                (limit.codepoint_index == codepoints.size()
                     ? codepoints.back().source_end()
                     : codepoints[limit.codepoint_index].source_start)) {
            return fail(
                LineBreakOpportunityErrorKind::InvalidGraphemeTopology,
                first.codepoint_index,
                cluster,
                "grapheme source offsets disagree with decoded code-point ranges",
                error);
        }
        const std::uint64_t cluster_codepoints =
            static_cast<std::uint64_t>(
                limit.codepoint_index - first.codepoint_index);
        const std::uint64_t cluster_bytes =
            limit.source_offset - first.source_offset;
        stats->maximum_cluster_codepoints = std::max(
            stats->maximum_cluster_codepoints,
            cluster_codepoints);
        stats->maximum_cluster_source_bytes = std::max(
            stats->maximum_cluster_source_bytes,
            cluster_bytes);
    }
    *cluster_count = count;
    stats->input_clusters = count;
    return true;
}

bool cluster_hard_break(
    const LineBreakOpportunityRequest& request,
    std::size_t cluster_index,
    bool* mandatory_after,
    LineBreakOpportunityError* error) noexcept {
    const GraphemeBoundary& first =
        request.grapheme_boundaries[cluster_index];
    const GraphemeBoundary& limit =
        request.grapheme_boundaries[cluster_index + 1U];
    const std::size_t first_index = first.codepoint_index;
    const std::size_t codepoint_count =
        limit.codepoint_index - first.codepoint_index;
    const LineBreakClass first_class =
        line_break_properties(request.codepoints[first_index].value).break_class;

    *mandatory_after = false;
    for (std::size_t offset = 0U; offset < codepoint_count; ++offset) {
        const LineBreakClass value = line_break_properties(
            request.codepoints[first_index + offset].value).break_class;
        if (!is_hard(value)) {
            continue;
        }
        const bool single_hard = offset == 0U && codepoint_count == 1U;
        const bool crlf =
            codepoint_count == 2U &&
            first_class == LineBreakClass::CR &&
            offset <= 1U &&
            line_break_properties(
                request.codepoints[first_index + 1U].value).break_class ==
                LineBreakClass::LF;
        if (!single_hard && !crlf) {
            return fail(
                LineBreakOpportunityErrorKind::InvalidGraphemeTopology,
                first_index + offset,
                cluster_index,
                "a grapheme cluster hides a mandatory line break",
                error);
        }
        *mandatory_after = true;
    }
    return true;
}

LineBreakClass cluster_last_raw_class(
    const LineBreakOpportunityRequest& request,
    std::size_t cluster_index) noexcept {
    const std::size_t codepoint_limit =
        request.grapheme_boundaries[cluster_index + 1U].codepoint_index;
    return line_break_properties(
        request.codepoints[codepoint_limit - 1U].value).break_class;
}

LineBreakOpportunity classify_boundary(
    const LineBreakOpportunityRequest& request,
    const std::pmr::vector<std::uint64_t>& units,
    std::size_t right_unit_index,
    std::uint64_t consecutive_ri) noexcept {
    const std::uint64_t left_unit = units[right_unit_index - 1U];
    const std::uint64_t right_unit = units[right_unit_index];
    const LineBreakClass left = unit_class(left_unit);
    const LineBreakClass right = unit_class(right_unit);
    const std::uint8_t left_flags = unit_flags(left_unit);
    const std::uint8_t right_flags = unit_flags(right_unit);

    // LB4-LB6. CR x LF has precedence over the mandatory CR break.
    if (left == LineBreakClass::CR && right == LineBreakClass::LF) {
        return LineBreakOpportunity::Prohibited;
    }
    if (unit_mandatory_after(left_unit) || is_hard(left)) {
        return LineBreakOpportunity::Mandatory;
    }
    if (is_hard(right)) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB7.
    if (right == LineBreakClass::SP || right == LineBreakClass::ZW) {
        return LineBreakOpportunity::Prohibited;
    }

    const std::size_t right_cluster = unit_cluster(right_unit);
    const LineBreakClass immediate_previous_raw =
        right_cluster == 0U
            ? LineBreakClass::AL
            : cluster_last_raw_class(request, right_cluster - 1U);

    std::size_t base_index = right_unit_index - 1U;
    while (base_index != 0U &&
           unit_class(units[base_index]) == LineBreakClass::SP) {
        --base_index;
    }
    const bool have_non_space_base =
        unit_class(units[base_index]) != LineBreakClass::SP;
    const LineBreakClass base =
        have_non_space_base ? unit_class(units[base_index])
                            : LineBreakClass::SP;
    const bool have_before_base =
        have_non_space_base && base_index != 0U;
    const LineBreakClass before_base =
        have_before_base ? unit_class(units[base_index - 1U])
                         : LineBreakClass::AL;

    // LB8.
    if (have_non_space_base && base == LineBreakClass::ZW) {
        return LineBreakOpportunity::Allowed;
    }

    // LB8a.
    if (immediate_previous_raw == LineBreakClass::ZWJ) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB11-LB12a.
    if (right == LineBreakClass::WJ || left == LineBreakClass::WJ ||
        left == LineBreakClass::GL) {
        return LineBreakOpportunity::Prohibited;
    }
    if (right == LineBreakClass::GL &&
        left != LineBreakClass::SP &&
        left != LineBreakClass::BA &&
        left != LineBreakClass::HY &&
        left != LineBreakClass::HH) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB13-LB14.
    if (right == LineBreakClass::CL ||
        right == LineBreakClass::CP ||
        right == LineBreakClass::EX ||
        right == LineBreakClass::SY) {
        return LineBreakOpportunity::Prohibited;
    }
    if (have_non_space_base && base == LineBreakClass::OP) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB15a.
    if (have_non_space_base &&
        base == LineBreakClass::QU &&
        is_initial_quote(unit_flags(units[base_index])) &&
        (!have_before_base ||
         before_base == LineBreakClass::BK ||
         before_base == LineBreakClass::CR ||
         before_base == LineBreakClass::LF ||
         before_base == LineBreakClass::NL ||
         before_base == LineBreakClass::OP ||
         before_base == LineBreakClass::QU ||
         before_base == LineBreakClass::GL ||
         before_base == LineBreakClass::SP ||
         before_base == LineBreakClass::ZW)) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB15b.
    if (right == LineBreakClass::QU && is_final_quote(right_flags)) {
        const bool at_end = right_unit_index + 1U == units.size();
        const LineBreakClass following = at_end
            ? LineBreakClass::AL
            : unit_class(units[right_unit_index + 1U]);
        if (at_end ||
            following == LineBreakClass::SP ||
            following == LineBreakClass::GL ||
            following == LineBreakClass::WJ ||
            following == LineBreakClass::CL ||
            following == LineBreakClass::QU ||
            following == LineBreakClass::CP ||
            following == LineBreakClass::EX ||
            following == LineBreakClass::IS ||
            following == LineBreakClass::SY ||
            following == LineBreakClass::BK ||
            following == LineBreakClass::CR ||
            following == LineBreakClass::LF ||
            following == LineBreakClass::NL ||
            following == LineBreakClass::ZW) {
            return LineBreakOpportunity::Prohibited;
        }
    }

    // LB15c-LB15d.
    if (left == LineBreakClass::SP &&
        right == LineBreakClass::IS &&
        right_unit_index + 1U < units.size() &&
        unit_class(units[right_unit_index + 1U]) == LineBreakClass::NU) {
        return LineBreakOpportunity::Allowed;
    }
    if (right == LineBreakClass::IS) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB16-LB17.
    if (right == LineBreakClass::NS &&
        have_non_space_base &&
        (base == LineBreakClass::CL || base == LineBreakClass::CP)) {
        return LineBreakOpportunity::Prohibited;
    }
    if (right == LineBreakClass::B2 &&
        have_non_space_base &&
        base == LineBreakClass::B2) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB18.
    if (left == LineBreakClass::SP) {
        return LineBreakOpportunity::Allowed;
    }

    // LB19.
    if (right == LineBreakClass::QU && !is_initial_quote(right_flags)) {
        return LineBreakOpportunity::Prohibited;
    }
    if (left == LineBreakClass::QU && !is_final_quote(left_flags)) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB19a.
    if (right == LineBreakClass::QU) {
        const bool following_east_asian =
            right_unit_index + 1U < units.size() &&
            is_east_asian(unit_flags(units[right_unit_index + 1U]));
        if (!is_east_asian(left_flags) || !following_east_asian) {
            return LineBreakOpportunity::Prohibited;
        }
    }
    if (left == LineBreakClass::QU) {
        const bool preceding_east_asian =
            right_unit_index >= 2U &&
            is_east_asian(unit_flags(units[right_unit_index - 2U]));
        if (!is_east_asian(right_flags) || !preceding_east_asian) {
            return LineBreakOpportunity::Prohibited;
        }
    }

    // LB20.
    if (right == LineBreakClass::CB || left == LineBreakClass::CB) {
        return LineBreakOpportunity::Allowed;
    }

    // LB20a.
    if ((left == LineBreakClass::HY || left == LineBreakClass::HH) &&
        is_alphabetic(right)) {
        const bool at_start = right_unit_index == 1U;
        const LineBreakClass preceding = at_start
            ? LineBreakClass::AL
            : unit_class(units[right_unit_index - 2U]);
        if (at_start ||
            preceding == LineBreakClass::BK ||
            preceding == LineBreakClass::CR ||
            preceding == LineBreakClass::LF ||
            preceding == LineBreakClass::NL ||
            preceding == LineBreakClass::SP ||
            preceding == LineBreakClass::ZW ||
            preceding == LineBreakClass::CB ||
            preceding == LineBreakClass::GL) {
            return LineBreakOpportunity::Prohibited;
        }
    }

    // LB21-LB22.
    if (right == LineBreakClass::BA ||
        right == LineBreakClass::HH ||
        right == LineBreakClass::HY ||
        right == LineBreakClass::NS ||
        left == LineBreakClass::BB) {
        return LineBreakOpportunity::Prohibited;
    }
    if ((left == LineBreakClass::HY || left == LineBreakClass::HH) &&
        right != LineBreakClass::HL &&
        right_unit_index >= 2U &&
        unit_class(units[right_unit_index - 2U]) == LineBreakClass::HL) {
        return LineBreakOpportunity::Prohibited;
    }
    if (left == LineBreakClass::SY && right == LineBreakClass::HL) {
        return LineBreakOpportunity::Prohibited;
    }
    if (right == LineBreakClass::IN) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB23-LB24.
    if ((is_alphabetic(left) && right == LineBreakClass::NU) ||
        (left == LineBreakClass::NU && is_alphabetic(right))) {
        return LineBreakOpportunity::Prohibited;
    }
    if ((left == LineBreakClass::PR &&
         (right == LineBreakClass::ID ||
          right == LineBreakClass::EB ||
          right == LineBreakClass::EM)) ||
        ((left == LineBreakClass::ID ||
          left == LineBreakClass::EB ||
          left == LineBreakClass::EM) &&
         right == LineBreakClass::PO)) {
        return LineBreakOpportunity::Prohibited;
    }
    if (((left == LineBreakClass::PR || left == LineBreakClass::PO) &&
         is_alphabetic(right)) ||
        (is_alphabetic(left) &&
         (right == LineBreakClass::PR || right == LineBreakClass::PO))) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB25.
    if (right == LineBreakClass::PO || right == LineBreakClass::PR) {
        std::size_t cursor = right_unit_index - 1U;
        if (unit_class(units[cursor]) == LineBreakClass::CL ||
            unit_class(units[cursor]) == LineBreakClass::CP) {
            if (cursor != 0U) {
                --cursor;
            } else {
                cursor = units.size();
            }
        }
        while (cursor < units.size() &&
               (unit_class(units[cursor]) == LineBreakClass::SY ||
                unit_class(units[cursor]) == LineBreakClass::IS)) {
            if (cursor == 0U) {
                cursor = units.size();
                break;
            }
            --cursor;
        }
        if (cursor < units.size() &&
            unit_class(units[cursor]) == LineBreakClass::NU) {
            return LineBreakOpportunity::Prohibited;
        }
    }
    if (left == LineBreakClass::PO || left == LineBreakClass::PR) {
        if (right == LineBreakClass::NU) {
            return LineBreakOpportunity::Prohibited;
        }
        if (right == LineBreakClass::OP) {
            if (right_unit_index + 1U < units.size() &&
                unit_class(units[right_unit_index + 1U]) ==
                    LineBreakClass::NU) {
                return LineBreakOpportunity::Prohibited;
            }
            if (right_unit_index + 2U < units.size() &&
                unit_class(units[right_unit_index + 1U]) ==
                    LineBreakClass::IS &&
                unit_class(units[right_unit_index + 2U]) ==
                    LineBreakClass::NU) {
                return LineBreakOpportunity::Prohibited;
            }
        }
    }
    if ((left == LineBreakClass::HY || left == LineBreakClass::IS) &&
        right == LineBreakClass::NU) {
        return LineBreakOpportunity::Prohibited;
    }
    if (right == LineBreakClass::NU) {
        std::size_t cursor = right_unit_index - 1U;
        while (unit_class(units[cursor]) == LineBreakClass::SY ||
               unit_class(units[cursor]) == LineBreakClass::IS) {
            if (cursor == 0U) {
                cursor = units.size();
                break;
            }
            --cursor;
        }
        if (cursor < units.size() &&
            unit_class(units[cursor]) == LineBreakClass::NU) {
            return LineBreakOpportunity::Prohibited;
        }
    }

    // LB26-LB27.
    if ((left == LineBreakClass::JL &&
         (right == LineBreakClass::JL ||
          right == LineBreakClass::JV ||
          right == LineBreakClass::H2 ||
          right == LineBreakClass::H3)) ||
        ((left == LineBreakClass::JV || left == LineBreakClass::H2) &&
         (right == LineBreakClass::JV || right == LineBreakClass::JT)) ||
        ((left == LineBreakClass::JT || left == LineBreakClass::H3) &&
         right == LineBreakClass::JT)) {
        return LineBreakOpportunity::Prohibited;
    }
    if ((is_hangul(left) && right == LineBreakClass::PO) ||
        (left == LineBreakClass::PR && is_hangul(right))) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB28.
    if (is_alphabetic(left) && is_alphabetic(right)) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB28a.
    const bool left_brahmic = is_brahmic_base(left, left_flags);
    const bool right_brahmic = is_brahmic_base(right, right_flags);
    if ((left == LineBreakClass::AP && right_brahmic) ||
        (left_brahmic &&
         (right == LineBreakClass::VF || right == LineBreakClass::VI))) {
        return LineBreakOpportunity::Prohibited;
    }
    if (left == LineBreakClass::VI &&
        is_aksara_or_dotted(right, right_flags) &&
        right_unit_index >= 2U &&
        is_brahmic_base(
            unit_class(units[right_unit_index - 2U]),
            unit_flags(units[right_unit_index - 2U]))) {
        return LineBreakOpportunity::Prohibited;
    }
    if (left_brahmic &&
        right_brahmic &&
        right_unit_index + 1U < units.size() &&
        unit_class(units[right_unit_index + 1U]) == LineBreakClass::VF) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB29-LB30.
    if (left == LineBreakClass::IS && is_alphabetic(right)) {
        return LineBreakOpportunity::Prohibited;
    }
    if ((left == LineBreakClass::AL ||
         left == LineBreakClass::HL ||
         left == LineBreakClass::NU) &&
        right == LineBreakClass::OP &&
        !is_east_asian(right_flags)) {
        return LineBreakOpportunity::Prohibited;
    }
    if (left == LineBreakClass::CP &&
        !is_east_asian(left_flags) &&
        (right == LineBreakClass::AL ||
         right == LineBreakClass::HL ||
         right == LineBreakClass::NU)) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB30a-LB30b.
    if (left == LineBreakClass::RI &&
        right == LineBreakClass::RI &&
        (consecutive_ri & 1U) != 0U) {
        return LineBreakOpportunity::Prohibited;
    }
    if ((left == LineBreakClass::EB ||
         is_extended_pictographic_unassigned(left_flags)) &&
        right == LineBreakClass::EM) {
        return LineBreakOpportunity::Prohibited;
    }

    // LB31.
    return LineBreakOpportunity::Allowed;
}

bool count_opportunities(
    const LineBreakOpportunityMap& map,
    LineBreakOpportunityStats* stats,
    LineBreakOpportunityError* error) noexcept {
    for (std::uint8_t raw : map.opportunities) {
        const auto value = static_cast<LineBreakOpportunity>(raw);
        std::uint64_t* counter = nullptr;
        switch (value) {
            case LineBreakOpportunity::Prohibited:
                counter = &stats->prohibited_boundaries;
                break;
            case LineBreakOpportunity::Allowed:
                counter = &stats->allowed_boundaries;
                break;
            case LineBreakOpportunity::Mandatory:
                counter = &stats->mandatory_boundaries;
                break;
        }
        if (counter == nullptr || !checked_add(counter, 1U)) {
            return fail(
                LineBreakOpportunityErrorKind::AggregateOverflow,
                0U,
                0U,
                "line-break opportunity statistics overflowed",
                error);
        }
    }
    return true;
}

} // namespace

LineBreakOpportunityMap::LineBreakOpportunityMap(
    std::pmr::memory_resource* resource)
    : opportunities(usable_resource(resource)) {}

std::pmr::memory_resource* LineBreakOpportunityMap::resource() const noexcept {
    return opportunities.get_allocator().resource();
}

void LineBreakOpportunityMap::release() noexcept {
    release_vector(&opportunities);
}

const char* line_break_opportunity_error_kind_name(
    LineBreakOpportunityErrorKind kind) noexcept {
    switch (kind) {
        case LineBreakOpportunityErrorKind::None:
            return "none";
        case LineBreakOpportunityErrorKind::InvalidCodePointStream:
            return "invalid_codepoint_stream";
        case LineBreakOpportunityErrorKind::InvalidGraphemeTopology:
            return "invalid_grapheme_topology";
        case LineBreakOpportunityErrorKind::ClusterDomainOverflow:
            return "cluster_domain_overflow";
        case LineBreakOpportunityErrorKind::WorkingMemoryBudgetExceeded:
            return "working_memory_budget_exceeded";
        case LineBreakOpportunityErrorKind::AggregateOverflow:
            return "aggregate_overflow";
    }
    return "invalid";
}

LineBreakOpportunity line_break_opportunity_at(
    const LineBreakOpportunityMap& map,
    std::uint32_t boundary_index) noexcept {
    const std::size_t index = static_cast<std::size_t>(boundary_index);
    if (index >= map.opportunities.size()) {
        return LineBreakOpportunity::Prohibited;
    }
    return static_cast<LineBreakOpportunity>(map.opportunities[index]);
}

bool build_line_break_opportunity_map(
    const LineBreakOpportunityRequest& request,
    LineBreakOpportunityMap* output,
    LineBreakOpportunityStats* stats,
    LineBreakOpportunityError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);

    LineBreakOpportunityStats working_stats;
    if (!validate_codepoint_stream(
            request.codepoints,
            &working_stats,
            error)) {
        return false;
    }
    std::size_t cluster_count = 0U;
    if (!validate_grapheme_topology(
            request,
            &cluster_count,
            &working_stats,
            error)) {
        return false;
    }

    LineBreakOpportunityMap working(output->resource());
    std::pmr::vector<std::uint64_t> units(output->resource());
    try {
        units.reserve(cluster_count);
        working.opportunities.resize(
            cluster_count + 1U,
            static_cast<std::uint8_t>(
                LineBreakOpportunity::Prohibited));
    } catch (const std::bad_alloc&) {
        return fail(
            LineBreakOpportunityErrorKind::WorkingMemoryBudgetExceeded,
            0U,
            0U,
            "line-break working set exceeds its hard budget",
            error);
    } catch (...) {
        return fail(
            LineBreakOpportunityErrorKind::WorkingMemoryBudgetExceeded,
            0U,
            0U,
            "line-break working-set allocation failed",
            error);
    }

    if (cluster_count == 0U) {
        working.opportunities[0] = static_cast<std::uint8_t>(
            LineBreakOpportunity::Mandatory);
        working_stats.output_boundaries = 1U;
        if (!count_opportunities(
                working,
                &working_stats,
                error)) {
            return false;
        }
        working.opportunities.swap(output->opportunities);
        *stats = working_stats;
        return true;
    }

    for (std::size_t cluster = 0U; cluster < cluster_count; ++cluster) {
        bool mandatory_after = false;
        if (!cluster_hard_break(
                request,
                cluster,
                &mandatory_after,
                error)) {
            return false;
        }
        if (mandatory_after &&
            !checked_add(&working_stats.hard_break_clusters, 1U)) {
            return fail(
                LineBreakOpportunityErrorKind::AggregateOverflow,
                request.grapheme_boundaries[cluster].codepoint_index,
                cluster,
                "hard-break cluster statistics overflowed",
                error);
        }

        const std::size_t codepoint_index =
            request.grapheme_boundaries[cluster].codepoint_index;
        const LineBreakProperties properties = line_break_properties(
            request.codepoints[codepoint_index].value);
        LineBreakClass effective = properties.break_class;
        std::uint8_t effective_flags = properties.flags;

        if ((effective == LineBreakClass::CM ||
             effective == LineBreakClass::ZWJ) &&
            !units.empty() &&
            is_lb9_base(unit_class(units.back()))) {
            if (!checked_add(
                    &working_stats.ignored_combining_clusters,
                    1U)) {
                return fail(
                    LineBreakOpportunityErrorKind::AggregateOverflow,
                    codepoint_index,
                    cluster,
                    "ignored-combining statistics overflowed",
                    error);
            }
            continue;
        }
        if (effective == LineBreakClass::CM ||
            effective == LineBreakClass::ZWJ) {
            effective = LineBreakClass::AL;
            effective_flags = 0U;
        }

        units.push_back(pack_unit(
            static_cast<std::uint32_t>(cluster),
            effective,
            effective_flags,
            mandatory_after));
    }

    if (units.empty()) {
        return fail(
            LineBreakOpportunityErrorKind::InvalidGraphemeTopology,
            0U,
            0U,
            "non-empty text did not produce a significant line-break unit",
            error);
    }

    working_stats.significant_clusters = units.size();
    std::uint64_t consecutive_ri =
        unit_class(units.front()) == LineBreakClass::RI ? 1U : 0U;
    for (std::size_t right = 1U; right < units.size(); ++right) {
        const std::size_t boundary =
            static_cast<std::size_t>(unit_cluster(units[right]));
        working.opportunities[boundary] =
            static_cast<std::uint8_t>(classify_boundary(
                request,
                units,
                right,
                consecutive_ri));

        const LineBreakClass left_class = unit_class(units[right - 1U]);
        const LineBreakClass right_class = unit_class(units[right]);
        if (right_class == LineBreakClass::RI) {
            consecutive_ri = left_class == LineBreakClass::RI
                ? consecutive_ri + 1U
                : 1U;
        } else {
            consecutive_ri = 0U;
        }
    }

    working.opportunities.back() = static_cast<std::uint8_t>(
        LineBreakOpportunity::Mandatory);
    working_stats.output_boundaries = working.opportunities.size();
    if (!count_opportunities(
            working,
            &working_stats,
            error)) {
        return false;
    }
    working.opportunities.swap(output->opportunities);
    *stats = working_stats;
    return true;
}

} // namespace zevryon::text
