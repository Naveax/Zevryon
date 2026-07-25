#include "line_break_opportunity.hpp"
#include "unicode_line_break_data.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using zevryon::text::DecodedCodePoint;
using zevryon::text::GraphemeBoundary;
using zevryon::text::LineBreakOpportunity;
using zevryon::text::LineBreakOpportunityError;
using zevryon::text::LineBreakOpportunityErrorKind;
using zevryon::text::LineBreakOpportunityMap;
using zevryon::text::LineBreakOpportunityRequest;
using zevryon::text::LineBreakOpportunityStats;

std::uint8_t utf8_length(std::uint32_t value) {
    return value <= 0x7FU ? 1U
         : value <= 0x7FFU ? 2U
         : value <= 0xFFFFU ? 3U
                            : 4U;
}

struct Fixture {
    std::vector<DecodedCodePoint> codepoints;
    std::vector<GraphemeBoundary> boundaries;
};

Fixture make_fixture(
    const std::vector<std::vector<std::uint32_t>>& clusters) {
    Fixture fixture;
    std::uint64_t source_offset = 0U;
    std::uint32_t codepoint_index = 0U;
    for (const auto& cluster : clusters) {
        fixture.boundaries.push_back(
            {source_offset, codepoint_index});
        for (std::uint32_t value : cluster) {
            const std::uint8_t length = utf8_length(value);
            fixture.codepoints.push_back(
                {value, source_offset, source_offset + length, false});
            source_offset += length;
            ++codepoint_index;
        }
    }
    if (!clusters.empty()) {
        fixture.boundaries.push_back(
            {source_offset, codepoint_index});
    }
    return fixture;
}

Fixture make_singletons(std::u32string text) {
    std::vector<std::vector<std::uint32_t>> clusters;
    clusters.reserve(text.size());
    for (char32_t value : text) {
        clusters.push_back(
            {static_cast<std::uint32_t>(value)});
    }
    return make_fixture(clusters);
}

std::vector<LineBreakOpportunity> build(
    const Fixture& fixture,
    LineBreakOpportunityStats* stats = nullptr) {
    LineBreakOpportunityMap map;
    LineBreakOpportunityStats local_stats;
    LineBreakOpportunityError error;
    const bool ok = zevryon::text::build_line_break_opportunity_map(
        {
            std::span<const DecodedCodePoint>(fixture.codepoints),
            std::span<const GraphemeBoundary>(fixture.boundaries)
        },
        &map,
        stats != nullptr ? stats : &local_stats,
        &error);
    if (!ok) {
        std::cerr << zevryon::text::line_break_opportunity_error_kind_name(
                         error.kind)
                  << ": " << error.message << '\n';
    }
    assert(ok);
    std::vector<LineBreakOpportunity> output;
    output.reserve(map.opportunities.size());
    for (std::uint8_t value : map.opportunities) {
        output.push_back(static_cast<LineBreakOpportunity>(value));
    }
    return output;
}

void expect(
    const Fixture& fixture,
    std::initializer_list<LineBreakOpportunity> expected) {
    const auto actual = build(fixture);
    assert(actual.size() == expected.size());
    assert(std::equal(actual.begin(), actual.end(), expected.begin()));
}

class LimitResource final : public std::pmr::memory_resource {
public:
    explicit LimitResource(std::size_t limit) : limit_(limit) {}

    std::size_t current() const noexcept { return current_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > limit_ - current_) {
            throw std::bad_alloc();
        }
        void* result =
            std::pmr::new_delete_resource()->allocate(bytes, alignment);
        current_ += bytes;
        return result;
    }

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(
            pointer,
            bytes,
            alignment);
        assert(current_ >= bytes);
        current_ -= bytes;
    }

    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t limit_;
    std::size_t current_{0};
};

void test_unicode_17_properties() {
    using namespace zevryon::text;
    assert(line_break_properties(U'A').break_class == LineBreakClass::AL);
    assert(line_break_properties(0x034FU).break_class == LineBreakClass::CM);
    assert(line_break_properties(0x058AU).break_class == LineBreakClass::HH);
    assert(line_break_properties(0x0E01U).break_class == LineBreakClass::AL);
    assert(line_break_properties(0x0E31U).break_class == LineBreakClass::CM);
    assert(line_break_properties(0x00ABU).break_class == LineBreakClass::QU);
    assert(
        (line_break_properties(0x00ABU).flags &
         kLineBreakInitialQuote) != 0U);
    assert(
        (line_break_properties(0x2329U).flags &
         kLineBreakEastAsian) != 0U);
    assert(
        (line_break_properties(0x1F02CU).flags &
         kLineBreakExtendedPictographicUnassigned) != 0U);
}

void test_empty_and_basic_western() {
    expect(
        {},
        {LineBreakOpportunity::Mandatory});
    expect(
        make_singletons(U"abc def"),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
}

void test_mandatory_and_crlf_cluster() {
    expect(
        make_fixture({{U'a'}, {0x000DU, 0x000AU}, {U'b'}}),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory,
            LineBreakOpportunity::Mandatory
        });
    expect(
        make_singletons(U"a\nb"),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory,
            LineBreakOpportunity::Mandatory
        });
}

void test_explicit_nonbreaks_and_zero_width_space() {
    expect(
        make_singletons(U"a\u2060b"),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
    expect(
        make_singletons(U"a\u00A0b"),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
    expect(
        make_singletons(U"a\u200Bb"),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Mandatory
        });
}

void test_east_asian_numbers_and_quotes() {
    expect(
        make_singletons(U"\u4E00\u4E8C\u4E09"),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Mandatory
        });
    expect(
        make_singletons(U"$12.50%"),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
    expect(
        make_singletons(U"A(\u00ABB\u00BB)"),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
}

void test_regional_indicators_emoji_hangul_and_brahmic() {
    expect(
        make_singletons(U"\U0001F1E6\U0001F1E7\U0001F1E8\U0001F1E9"),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
    expect(
        make_singletons(U"\u261D\U0001F3FB"),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
    expect(
        make_singletons(U"\u1100\u1160\u11A8"),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
    expect(
        make_singletons(U"\U00011003\u1B05\u1B44\u1B05"),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
}

void test_grapheme_tailoring_and_lb9() {
    expect(
        make_fixture({{0x0020U, 0x0301U}, {U'A'}}),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Allowed,
            LineBreakOpportunity::Mandatory
        });
    expect(
        make_fixture({{U'A'}, {0x0000U}, {U'B'}}),
        {
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Prohibited,
            LineBreakOpportunity::Mandatory
        });
}

void test_failure_atomic_budget_and_invalid_topology() {
    const Fixture fixture = make_singletons(U"abc def");
    LimitResource resource(1U);
    LineBreakOpportunityMap map(&resource);
    map.opportunities.push_back(
        static_cast<std::uint8_t>(LineBreakOpportunity::Allowed));
    LineBreakOpportunityStats stats;
    LineBreakOpportunityError error;
    const bool ok = zevryon::text::build_line_break_opportunity_map(
        {
            std::span<const DecodedCodePoint>(fixture.codepoints),
            std::span<const GraphemeBoundary>(fixture.boundaries)
        },
        &map,
        &stats,
        &error);
    assert(!ok);
    assert(
        error.kind ==
        LineBreakOpportunityErrorKind::WorkingMemoryBudgetExceeded);
    assert(map.opportunities.empty());
    assert(resource.current() == 0U);

    const Fixture invalid =
        make_fixture({{U'A', 0x000AU}, {U'B'}});
    LineBreakOpportunityMap invalid_map;
    assert(!zevryon::text::build_line_break_opportunity_map(
        {
            std::span<const DecodedCodePoint>(invalid.codepoints),
            std::span<const GraphemeBoundary>(invalid.boundaries)
        },
        &invalid_map,
        &stats,
        &error));
    assert(
        error.kind ==
        LineBreakOpportunityErrorKind::InvalidGraphemeTopology);
    assert(invalid_map.opportunities.empty());
}

void test_exact_stats() {
    const Fixture fixture = make_singletons(U"abc def");
    LineBreakOpportunityStats stats;
    const auto output = build(fixture, &stats);
    assert(output.size() == 8U);
    assert(stats.input_codepoints == 7U);
    assert(stats.input_clusters == 7U);
    assert(stats.output_boundaries == 8U);
    assert(stats.significant_clusters == 7U);
    assert(stats.ignored_combining_clusters == 0U);
    assert(stats.mandatory_boundaries == 1U);
    assert(stats.allowed_boundaries == 1U);
    assert(stats.prohibited_boundaries == 6U);
}

} // namespace

int main() {
    test_unicode_17_properties();
    test_empty_and_basic_western();
    test_mandatory_and_crlf_cluster();
    test_explicit_nonbreaks_and_zero_width_space();
    test_east_asian_numbers_and_quotes();
    test_regional_indicators_emoji_hangul_and_brahmic();
    test_grapheme_tailoring_and_lb9();
    test_failure_atomic_budget_and_invalid_topology();
    test_exact_stats();
    std::cout << "line break opportunity tests passed\n";
    return 0;
}
