#include "viewport_projection.hpp"

#include <cstdint>
#include <iostream>
#include <limits>

namespace {
using namespace zevryon::text;

std::uint64_t distance(
    std::int64_t left,
    std::int64_t right) noexcept {
    if (left >= right) {
        return static_cast<std::uint64_t>(left) -
               static_cast<std::uint64_t>(right);
    }
    return static_cast<std::uint64_t>(right) -
           static_cast<std::uint64_t>(left);
}

struct OracleResult final {
    std::uint32_t line{0};
    std::uint32_t fragment{0};
    std::uint32_t boundary{0};
    std::uint32_t flags{0};
    std::uint64_t inline_distance{0};
    std::uint64_t block_distance{0};
};

bool oracle_hit(
    const ViewportProjection& projection,
    std::int64_t x,
    std::int64_t y,
    ViewportHitTestBias bias,
    OracleResult* output) {
    std::size_t selected_line = projection.lines.size();
    std::uint64_t block_distance =
        std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0U;
         index < projection.lines.size();
         ++index) {
        const ViewportLineRecord& line = projection.lines[index];
        if (line.caret_count == 0U) {
            continue;
        }
        const std::int64_t line_end = line.viewport_block_start +
            static_cast<std::int64_t>(line.block_size);
        std::uint64_t candidate = 0U;
        if (y < line.viewport_block_start) {
            candidate = distance(y, line.viewport_block_start);
        } else if (y > line_end) {
            candidate = distance(y, line_end);
        }
        if (candidate < block_distance) {
            block_distance = candidate;
            selected_line = index;
        }
    }
    if (selected_line == projection.lines.size()) {
        return false;
    }

    const ViewportLineRecord& line = projection.lines[selected_line];
    const ViewportCaretEdge* selected_caret = nullptr;
    std::uint64_t inline_distance =
        std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = line.first_caret;
         index < static_cast<std::size_t>(line.first_caret) +
                     line.caret_count;
         ++index) {
        const ViewportCaretEdge& caret = projection.carets[index];
        const std::uint64_t candidate = distance(
            x,
            caret.viewport_inline_position);
        bool replace = candidate < inline_distance;
        if (candidate == inline_distance && selected_caret != nullptr) {
            if (bias == ViewportHitTestBias::TowardVisualEnd) {
                replace = caret.viewport_inline_position >=
                    selected_caret->viewport_inline_position;
            } else {
                replace = caret.viewport_inline_position <
                    selected_caret->viewport_inline_position;
            }
        }
        if (replace) {
            selected_caret = &caret;
            inline_distance = candidate;
        }
    }
    if (selected_caret == nullptr) {
        return false;
    }

    output->line = line.source_line_index;
    output->fragment = selected_caret->source_fragment_index;
    output->boundary = selected_caret->boundary_index;
    output->inline_distance = inline_distance;
    output->block_distance = block_distance;
    const ViewportCaretEdge& first =
        projection.carets[line.first_caret];
    const ViewportCaretEdge& last = projection.carets[
        static_cast<std::size_t>(line.first_caret) +
        line.caret_count - 1U];
    if (x < first.viewport_inline_position ||
        x > last.viewport_inline_position) {
        output->flags |= kViewportHitClampedInline;
    }
    if (block_distance != 0U) {
        output->flags |= kViewportHitClampedBlock;
    }
    return true;
}

} // namespace

int main() {
    using namespace zevryon::text;
    std::uint64_t cases = 0U;
    for (std::uint32_t line_count = 1U;
         line_count <= 4U;
         ++line_count) {
        for (std::uint32_t caret_count = 1U;
             caret_count <= 6U;
             ++caret_count) {
            ViewportProjection projection;
            std::uint32_t first_caret = 0U;
            for (std::uint32_t line_index = 0U;
                 line_index < line_count;
                 ++line_index) {
                const std::uint32_t local_count =
                    line_index == 1U && line_count > 2U
                        ? 0U
                        : caret_count;
                projection.lines.push_back(ViewportLineRecord{
                    static_cast<std::int64_t>(line_index * 20U),
                    static_cast<std::int64_t>(line_index * 20U + 15U),
                    20U,
                    100U,
                    line_index,
                    0U,
                    0U,
                    first_caret,
                    local_count,
                    0U,
                    0U,
                    0U});
                for (std::uint32_t caret_index = 0U;
                     caret_index < local_count;
                     ++caret_index) {
                    const std::int64_t position =
                        static_cast<std::int64_t>(
                            (caret_index / 2U) * 10U);
                    projection.carets.push_back(ViewportCaretEdge{
                        position,
                        static_cast<std::int64_t>(line_index * 20U),
                        20U,
                        line_index * 100U + caret_index,
                        line_index * 10U + caret_index,
                        0U,
                        0U});
                }
                first_caret += local_count;
            }

            for (std::int64_t y = -10;
                 y <= static_cast<std::int64_t>(
                     line_count * 20U + 10U);
                 y += 3) {
                for (std::int64_t x = -7; x <= 40; x += 2) {
                    for (const ViewportHitTestBias bias : {
                             ViewportHitTestBias::Nearest,
                             ViewportHitTestBias::TowardVisualStart,
                             ViewportHitTestBias::TowardVisualEnd}) {
                        OracleResult expected;
                        ViewportHitTestResult actual;
                        if (!oracle_hit(
                                projection,
                                x,
                                y,
                                bias,
                                &expected) ||
                            !hit_test_viewport_projection(
                                projection,
                                x,
                                y,
                                bias,
                                &actual) ||
                            actual.source_line_index != expected.line ||
                            actual.source_fragment_index !=
                                expected.fragment ||
                            actual.boundary_index != expected.boundary ||
                            actual.flags != expected.flags ||
                            actual.inline_distance !=
                                expected.inline_distance ||
                            actual.block_distance !=
                                expected.block_distance) {
                            std::cerr <<
                                "Viewport hit-test equivalence mismatch\n";
                            return 1;
                        }
                        ++cases;
                    }
                }
            }
        }
    }
    std::cout << "Viewport hit-test equivalence passed: "
              << cases << " cases\n";
    return 0;
}
