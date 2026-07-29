#include "shared_pixel_compositor.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace zevryon::text;

std::uint8_t mul255(std::uint8_t left, std::uint8_t right) {
    return static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(left) * right + 127U) / 255U);
}

std::array<std::uint8_t, 4U> expected_pixel(
    std::uint8_t coverage,
    std::uint8_t style_channel,
    std::uint8_t background_channel) {
    const std::uint8_t alpha = coverage;
    const std::uint8_t source = mul255(style_channel, alpha);
    const std::uint8_t inverse = static_cast<std::uint8_t>(255U - alpha);
    const std::uint8_t output = static_cast<std::uint8_t>(
        std::min<std::uint32_t>(
            255U,
            static_cast<std::uint32_t>(source) +
                mul255(background_channel, inverse)));
    return {output, output, output, 255U};
}

} // namespace

int main() {
    using namespace zevryon::text;

    GpuFrameSubmission frame;
    frame.surface.surface_id = 1U;
    frame.surface.generation_id = 2U;
    frame.surface.width = 1U;
    frame.surface.height = 1U;
    frame.surface.format = GpuSurfaceFormat::Rgba8Unorm;
    frame.surface.premultiplied_alpha = 1U;
    frame.frame_id = 3U;
    frame.atlas_generation_id = 4U;
    frame.clips.push_back(TextPaintClipRect{0, 0, 1U, 1U});
    frame.page_references.push_back(GpuFramePageReference{
        5U, 0U, 0U, 0U, 1U, GlyphRasterFormat::Alpha8, 0U, 0U});
    frame.glyph_batches.push_back(GpuFrameGlyphBatch{
        5U, 0U, 0U, 1U, 10U, 0U, 0U, 0U});
    frame.commands.push_back(GpuFrameCommandRecord{
        GpuFrameCommandKind::GlyphBatch, 0U, 0U, 0U});

    GlyphAtlasDrawInstance instance;
    instance.viewport_inline_start = 0;
    instance.viewport_block_start = 0;
    instance.atlas_generation_id = 4U;
    instance.page_generation = 5U;
    instance.page_index = 0U;
    instance.width = 1U;
    instance.height = 1U;
    instance.style_id = 10U;
    instance.clip_index = 0U;
    std::array<GlyphAtlasDrawInstance, 1U> instances{instance};

    GlyphAtlasSubmission atlas;
    atlas.atlas_generation_id = 4U;
    atlas.submission_epoch = 1U;
    atlas.uploads.push_back(GlyphAtlasUploadRecord{
        4U, 5U, 0U, 1U, 0U, 0U, 0U, 1U, 1U, 1U, 0U,
        GlyphRasterFormat::Alpha8, 0U, 0U});

    SharedPixelCompositor compositor(
        SharedPixelCompositorConfig{
            1U, 1U, 1U, 2U, 4U, 4U});
    SharedCompositedFrame output;
    SharedPixelCompositorStats stats;
    SharedPixelCompositorError error;
    std::array<std::byte, 1U> payload{};
    std::array<SharedPixelStyle, 1U> styles{};

    std::uint64_t cases = 0U;
    for (std::uint32_t coverage_step = 0U;
         coverage_step < 16U; ++coverage_step) {
        const std::uint8_t coverage =
            static_cast<std::uint8_t>(coverage_step * 17U);
        payload[0] = static_cast<std::byte>(coverage);
        for (std::uint32_t style_step = 0U;
             style_step < 16U; ++style_step) {
            const std::uint8_t style =
                static_cast<std::uint8_t>(style_step * 17U);
            styles[0] = SharedPixelStyle{
                10U,
                (static_cast<std::uint32_t>(style) << 24U) |
                (static_cast<std::uint32_t>(style) << 16U) |
                (static_cast<std::uint32_t>(style) << 8U) |
                0xFFU};
            for (std::uint32_t background_step = 0U;
                 background_step < 16U; ++background_step) {
                const std::uint8_t background =
                    static_cast<std::uint8_t>(background_step * 17U);
                const std::uint32_t background_rgba =
                    (static_cast<std::uint32_t>(background) << 24U) |
                    (static_cast<std::uint32_t>(background) << 16U) |
                    (static_cast<std::uint32_t>(background) << 8U) |
                    0xFFU;
                SharedPixelCompositorRequest request;
                request.frame = &frame;
                request.atlas_submission = &atlas;
                request.draw_instances = instances;
                request.raster_payload = payload;
                request.styles = styles;
                request.background_rgba8 = background_rgba;
                request.limits.maximum_commands = 1U;
                request.limits.maximum_fill_rects = 1U;
                request.limits.maximum_glyph_batches = 1U;
                request.limits.maximum_draw_instances = 1U;
                request.limits.maximum_upload_bytes = 1U;
                assert(compose_shared_pixel_frame(
                    request, &compositor, &output, &stats, &error));
                const auto expected =
                    expected_pixel(coverage, style, background);
                assert(output.pixels.size() == 4U);
                assert(std::to_integer<std::uint8_t>(output.pixels[0]) ==
                       expected[0]);
                assert(std::to_integer<std::uint8_t>(output.pixels[1]) ==
                       expected[1]);
                assert(std::to_integer<std::uint8_t>(output.pixels[2]) ==
                       expected[2]);
                assert(std::to_integer<std::uint8_t>(output.pixels[3]) ==
                       expected[3]);
                cases += 1U;
            }
        }
    }
    assert(cases == 4096U);
    return 0;
}
