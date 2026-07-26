#include "glyph_atlas_submission.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace zevryon::text;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

std::uint64_t checksum(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const std::byte value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

bool key_less(const GlyphRasterKey& left, const GlyphRasterKey& right) noexcept {
    return std::tie(
               left.font_generation_id,
               left.face_id,
               left.glyph_id,
               left.x_scale,
               left.y_scale,
               left.mode,
               left.subpixel_x,
               left.subpixel_y) <
        std::tie(
               right.font_generation_id,
               right.face_id,
               right.glyph_id,
               right.x_scale,
               right.y_scale,
               right.mode,
               right.subpixel_x,
               right.subpixel_y);
}

std::vector<std::uint32_t> decode_pattern(
    std::uint32_t encoded,
    std::uint32_t length) {
    std::vector<std::uint32_t> values(length);
    for (std::uint32_t index = 0U; index < length; ++index) {
        values[index] = encoded % 3U;
        encoded /= 3U;
    }
    return values;
}

std::uint32_t power3(std::uint32_t exponent) {
    std::uint32_t value = 1U;
    for (std::uint32_t index = 0U; index < exponent; ++index) {
        value *= 3U;
    }
    return value;
}

bool certify_working_set_oracle(std::uint64_t* passed) {
    for (std::uint32_t length = 1U; length <= 6U; ++length) {
        const std::uint32_t patterns = power3(length);
        for (std::uint32_t encoded = 0U; encoded < patterns; ++encoded) {
            const std::vector<std::uint32_t> values =
                decode_pattern(encoded, length);
            for (std::uint32_t direction_index = 0U;
                 direction_index < 2U;
                 ++direction_index) {
                for (std::uint32_t mode_index = 0U;
                     mode_index < 3U;
                     ++mode_index) {
                    MultiRunShapedText shaped;
                    shaped.segments.emplace_back(
                        std::pmr::get_default_resource());
                    MultiRunShapedSegment& segment = shaped.segments.back();
                    segment.run.face_id = 19U;
                    segment.run.direction = direction_index == 0U
                        ? ShapingDirection::LeftToRight
                        : ShapingDirection::RightToLeft;
                    segment.glyphs.direction = segment.run.direction;
                    segment.glyphs.x_scale = 64;
                    segment.glyphs.y_scale = 64;
                    for (std::uint32_t index = 0U;
                         index < length;
                         ++index) {
                        const std::uint32_t value = values[index];
                        const std::int32_t magnitude =
                            static_cast<std::int32_t>(7U + value);
                        const std::int32_t advance = direction_index == 0U
                            ? magnitude
                            : -magnitude;
                        segment.glyphs.glyphs.push_back({
                            100U + value,
                            index,
                            advance,
                            0,
                            static_cast<std::int32_t>(value) - 1,
                            static_cast<std::int32_t>(1U - value),
                            0U});
                    }

                    TextPaintCommandStream paint;
                    TextPaintGlyphBatch batch;
                    batch.viewport_inline_origin = direction_index == 0U
                        ? 1'000
                        : 2'000;
                    batch.viewport_baseline = 500;
                    batch.segment_index = 0U;
                    batch.glyph_count = length;
                    batch.style_id = 41U;
                    batch.face_id = 19U;
                    batch.x_scale = 64;
                    batch.y_scale = 64;
                    batch.source_line_index = 7U;
                    if (direction_index != 0U) {
                        batch.flags = kTextPaintGlyphBatchRtl;
                    }
                    paint.glyph_batches.push_back(batch);
                    paint.commands.push_back({
                        TextPaintCommandKind::GlyphBatch,
                        0U,
                        0U,
                        0U});

                    const std::array<std::uint64_t, 1> generations{77U};
                    std::array<GlyphRasterConfig, 1> configs{};
                    configs[0].mode = static_cast<GlyphRasterMode>(mode_index);
                    configs[0].subpixel_x = 3U;
                    configs[0].subpixel_y = 5U;

                    GlyphRasterWorkingSet output;
                    GlyphRasterWorkingSetError error;
                    const GlyphRasterWorkingSetRequest request{
                        &paint,
                        &shaped,
                        generations,
                        configs,
                        {3U, 6U}};
                    if (!require(
                            build_glyph_raster_working_set(
                                request,
                                &output,
                                nullptr,
                                &error),
                            error.message) ||
                        !require(
                            output.uses.size() == length,
                            "oracle use count")) {
                        return false;
                    }

                    std::vector<GlyphRasterKey> expected_keys;
                    expected_keys.reserve(length);
                    std::int64_t pen = batch.viewport_inline_origin;
                    for (std::uint32_t index = 0U;
                         index < length;
                         ++index) {
                        const std::uint32_t value = values[index];
                        GlyphRasterKey key;
                        key.font_generation_id = 77U;
                        key.face_id = 19U;
                        key.glyph_id = 100U + value;
                        key.x_scale = 64;
                        key.y_scale = 64;
                        key.mode = static_cast<GlyphRasterMode>(mode_index);
                        key.subpixel_x = 3U;
                        key.subpixel_y = 5U;
                        expected_keys.push_back(key);

                        const GlyphRasterUseRecord& use = output.uses[index];
                        const std::int64_t expected_inline = pen +
                            static_cast<std::int64_t>(value) - 1;
                        const std::int64_t expected_baseline = 500 -
                            (static_cast<std::int64_t>(1U) -
                             static_cast<std::int64_t>(value));
                        if (!require(
                                use.viewport_inline_origin == expected_inline,
                                "oracle inline pen/offset") ||
                            !require(
                                use.viewport_baseline_origin == expected_baseline,
                                "oracle baseline offset") ||
                            !require(
                                use.glyph_index == index,
                                "oracle glyph index") ||
                            !require(
                                ((use.flags & kGlyphRasterUseRtl) != 0U) ==
                                    (direction_index != 0U),
                                "oracle direction flag")) {
                            return false;
                        }
                        const std::int64_t magnitude = 7 +
                            static_cast<std::int64_t>(value);
                        pen += direction_index == 0U ? magnitude : -magnitude;
                    }
                    std::sort(
                        expected_keys.begin(),
                        expected_keys.end(),
                        key_less);
                    expected_keys.erase(
                        std::unique(
                            expected_keys.begin(),
                            expected_keys.end()),
                        expected_keys.end());
                    if (!require(
                            output.entries.size() == expected_keys.size(),
                            "oracle unique-key count")) {
                        return false;
                    }
                    for (std::size_t index = 0U;
                         index < expected_keys.size();
                         ++index) {
                        if (!require(
                                output.entries[index].key == expected_keys[index],
                                "oracle sorted raster key")) {
                            return false;
                        }
                    }
                    for (std::uint32_t index = 0U;
                         index < length;
                         ++index) {
                        const GlyphRasterKey& actual_key =
                            output.entries[output.uses[index].key_index].key;
                        if (!require(
                                actual_key.glyph_id == 100U + values[index],
                                "oracle use-to-key mapping")) {
                            return false;
                        }
                    }
                    ++*passed;
                }
            }
        }
    }
    return true;
}

bool certify_batch_oracle(std::uint64_t* passed) {
    for (std::uint32_t length = 1U; length <= 6U; ++length) {
        const std::uint32_t patterns = power3(length);
        for (std::uint32_t encoded = 0U; encoded < patterns; ++encoded) {
            const std::vector<std::uint32_t> values =
                decode_pattern(encoded, length);
            GlyphRasterWorkingSet working_set;
            std::vector<GlyphRasterSourceRecord> sources;
            std::vector<std::byte> payload;
            working_set.entries.reserve(length);
            working_set.uses.reserve(length);
            sources.reserve(length);

            for (std::uint32_t index = 0U; index < length; ++index) {
                GlyphRasterWorkingSetEntry entry;
                entry.key.font_generation_id = 9U;
                entry.key.face_id = 4U;
                entry.key.glyph_id = 1'000U + index;
                entry.key.x_scale = 64;
                entry.key.y_scale = 64;
                entry.key.mode = static_cast<GlyphRasterMode>(values[index]);
                entry.first_use_index = index;
                entry.use_count = 1U;
                working_set.entries.push_back(entry);

                GlyphRasterUseRecord use;
                use.viewport_inline_origin =
                    static_cast<std::int64_t>(index * 12U);
                use.viewport_baseline_origin = 20;
                use.key_index = index;
                use.style_id = index % 2U;
                use.clip_index = (index / 2U) % 2U;
                use.source_line_index = index / 3U;
                working_set.uses.push_back(use);

                GlyphRasterSourceRecord source;
                source.key = entry.key;
                source.width = 4U;
                source.height = 4U;
                source.bearing_y = 3;
                const GlyphRasterMode mode = entry.key.mode;
                source.format = mode == GlyphRasterMode::Grayscale
                    ? GlyphRasterFormat::Alpha8
                    : (mode == GlyphRasterMode::Lcd
                        ? GlyphRasterFormat::LcdRgb8
                        : GlyphRasterFormat::Bgra8);
                const std::uint32_t bytes_per_pixel =
                    mode == GlyphRasterMode::Grayscale ? 1U
                    : (mode == GlyphRasterMode::Lcd ? 3U : 4U);
                source.row_bytes = source.width * bytes_per_pixel;
                source.payload_offset = payload.size();
                source.payload_size =
                    static_cast<std::uint64_t>(source.row_bytes) * source.height;
                for (std::uint64_t byte = 0U;
                     byte < source.payload_size;
                     ++byte) {
                    payload.push_back(static_cast<std::byte>(
                        (index * 23U + static_cast<std::uint32_t>(byte)) & 0xffU));
                }
                source.content_checksum = checksum(
                    std::span<const std::byte>(payload).subspan(
                        static_cast<std::size_t>(source.payload_offset),
                        static_cast<std::size_t>(source.payload_size)));
                sources.push_back(source);
            }

            GlyphAtlasCache cache(
                {64U, 64U, 3U, 6U, 1U, 0U},
                1U << 20U);
            GlyphAtlasSubmission submission;
            GlyphAtlasSubmissionError error;
            const GlyphAtlasSubmissionRequest request{
                &working_set,
                sources,
                payload,
                {6U, payload.size(), 6U, 6U}};
            if (!require(
                    prepare_glyph_atlas_submission(
                        request,
                        &cache,
                        &submission,
                        nullptr,
                        &error),
                    error.message) ||
                !require(
                    submission.draw_instances.size() == length,
                    "batch oracle instance count")) {
                return false;
            }

            std::vector<GlyphAtlasDrawBatch> expected;
            for (std::uint32_t first = 0U; first < length;) {
                const GlyphAtlasDrawInstance& first_instance =
                    submission.draw_instances[first];
                std::uint32_t limit = first + 1U;
                while (limit < length) {
                    const GlyphAtlasDrawInstance& next =
                        submission.draw_instances[limit];
                    if (next.page_generation != first_instance.page_generation ||
                        next.page_index != first_instance.page_index ||
                        next.style_id != first_instance.style_id ||
                        next.clip_index != first_instance.clip_index) {
                        break;
                    }
                    ++limit;
                }
                GlyphAtlasDrawBatch batch;
                batch.page_generation = first_instance.page_generation;
                batch.page_index = first_instance.page_index;
                batch.style_id = first_instance.style_id;
                batch.clip_index = first_instance.clip_index;
                batch.first_instance = first;
                batch.instance_count = limit - first;
                if (batch.instance_count > 1U) {
                    batch.flags = kGlyphAtlasDrawBatchCoalesced;
                }
                expected.push_back(batch);
                first = limit;
            }
            if (!require(
                    submission.draw_batches.size() == expected.size(),
                    "batch oracle run count")) {
                return false;
            }
            for (std::size_t index = 0U; index < expected.size(); ++index) {
                if (!require(
                        submission.draw_batches[index] == expected[index],
                        "batch oracle maximal run")) {
                    return false;
                }
            }
            ++*passed;
        }
    }
    return true;
}

} // namespace

int main() {
    std::uint64_t passed = 0U;
    if (!certify_working_set_oracle(&passed) ||
        !certify_batch_oracle(&passed)) {
        return 1;
    }
    constexpr std::uint64_t kExpectedCases = 7'644U;
    if (!require(passed == kExpectedCases, "exact oracle case count")) {
        return 1;
    }
    std::cout << "glyph atlas equivalence: " << passed << "/"
              << kExpectedCases << " PASS\n";
    return 0;
}
