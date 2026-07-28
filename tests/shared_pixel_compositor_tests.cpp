#include "shared_pixel_compositor.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

namespace {

using namespace zevryon::text;

struct Fixture final {
    GpuFrameSubmission frame;
    GlyphAtlasSubmission atlas;
    std::vector<GlyphAtlasDrawInstance> instances;
    std::vector<std::byte> payload;
    std::array<SharedPixelStyle, 3U> styles{{
        {1U, 0x2050C080U},
        {2U, 0xF0E0D0FFU},
        {3U, 0xFF3020FFU}}};

    Fixture() {
        frame.surface.surface_id = 10U;
        frame.surface.generation_id = 20U;
        frame.surface.width = 64U;
        frame.surface.height = 32U;
        frame.surface.format = GpuSurfaceFormat::Bgra8Unorm;
        frame.surface.premultiplied_alpha = 1U;
        frame.frame_id = 30U;
        frame.atlas_generation_id = 40U;
        frame.atlas_submission_epoch = 50U;
        frame.clips.push_back(TextPaintClipRect{0, 0, 64U, 32U});

        frame.fill_rects.push_back(TextPaintFillRect{
            2, 3, 18U, 8U, 1U, 0U, 0U, kTextPaintRectSelection});
        frame.fill_rects.push_back(TextPaintFillRect{
            55, 2, 2U, 20U, 3U, 0U, 0U, kTextPaintRectCaret});

        frame.page_references.push_back(GpuFramePageReference{
            100U, 0U, 0U, 0U, 1U, GlyphRasterFormat::Alpha8, 0U, 0U});
        frame.page_references.push_back(GpuFramePageReference{
            101U, 0U, 1U, 1U, 1U, GlyphRasterFormat::LcdRgb8, 0U, 0U});
        frame.page_references.push_back(GpuFramePageReference{
            102U, 0U, 2U, 2U, 1U, GlyphRasterFormat::Bgra8, 0U, 0U});

        for (std::uint32_t page = 0U; page < 3U; ++page) {
            GlyphAtlasDrawInstance instance;
            instance.viewport_inline_start =
                10 + static_cast<std::int64_t>(page) * 12;
            instance.viewport_block_start = 12;
            instance.atlas_generation_id = 40U;
            instance.page_generation = 100U + page;
            instance.page_index = page;
            instance.atlas_x = 0U;
            instance.atlas_y = 0U;
            instance.width = 4U;
            instance.height = 4U;
            instance.style_id = 2U;
            instance.clip_index = 0U;
            instance.working_set_key_index = page;
            instances.push_back(instance);

            frame.glyph_batches.push_back(GpuFrameGlyphBatch{
                100U + page,
                page,
                page,
                1U,
                2U,
                0U,
                page,
                0U});
        }

        frame.commands.push_back(GpuFrameCommandRecord{
            GpuFrameCommandKind::FillRect, 0U, 0U, 0U});
        for (std::uint32_t batch = 0U; batch < 3U; ++batch) {
            frame.commands.push_back(GpuFrameCommandRecord{
                GpuFrameCommandKind::GlyphBatch, batch, 0U, 0U});
        }
        frame.commands.push_back(GpuFrameCommandRecord{
            GpuFrameCommandKind::FillRect, 1U, 0U, 0U});

        atlas.atlas_generation_id = 40U;
        atlas.submission_epoch = 50U;

        const auto append_upload = [this](
            std::uint32_t page,
            std::uint64_t generation,
            GlyphRasterFormat format,
            std::uint32_t row_bytes,
            std::span<const std::byte> bytes) {
            const std::uint64_t offset = payload.size();
            payload.insert(payload.end(), bytes.begin(), bytes.end());
            atlas.uploads.push_back(GlyphAtlasUploadRecord{
                40U,
                generation,
                offset,
                bytes.size(),
                page,
                0U,
                0U,
                4U,
                4U,
                row_bytes,
                page,
                format,
                0U,
                0U});
        };

        std::array<std::byte, 16U> alpha{};
        for (std::size_t index = 0U; index < alpha.size(); ++index) {
            alpha[index] = static_cast<std::byte>(
                32U + static_cast<std::uint32_t>(index) * 14U);
        }
        append_upload(0U, 100U, GlyphRasterFormat::Alpha8, 4U, alpha);

        std::array<std::byte, 48U> lcd{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            lcd[index * 3U + 0U] = std::byte{0xFF};
            lcd[index * 3U + 1U] = std::byte{0x80};
            lcd[index * 3U + 2U] = std::byte{0x40};
        }
        append_upload(1U, 101U, GlyphRasterFormat::LcdRgb8, 12U, lcd);

        std::array<std::byte, 64U> color{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            color[index * 4U + 0U] = std::byte{0x20};
            color[index * 4U + 1U] = std::byte{0xD0};
            color[index * 4U + 2U] = std::byte{0x40};
            color[index * 4U + 3U] = std::byte{0xA0};
        }
        append_upload(2U, 102U, GlyphRasterFormat::Bgra8, 16U, color);
    }

    SharedPixelCompositorRequest request() const {
        SharedPixelCompositorRequest request;
        request.frame = &frame;
        request.atlas_submission = &atlas;
        request.draw_instances = instances;
        request.raster_payload = payload;
        request.styles = styles;
        request.background_rgba8 = 0x101820FFU;
        request.limits.maximum_commands = 16U;
        request.limits.maximum_fill_rects = 8U;
        request.limits.maximum_glyph_batches = 8U;
        request.limits.maximum_draw_instances = 16U;
        request.limits.maximum_upload_bytes = 4096U;
        return request;
    }
};

SharedPixelCompositor make_compositor() {
    return SharedPixelCompositor(
        SharedPixelCompositorConfig{
            16U, 16U, 3U, 16U,
            64U * 32U * 4U,
            16U * 16U * 4U * 3U});
}

void test_cold_and_hot_composition() {
    Fixture fixture;
    SharedPixelCompositor compositor = make_compositor();
    SharedCompositedFrame output;
    SharedPixelCompositorStats stats;
    SharedPixelCompositorError error;
    const SharedPixelCompositorRequest cold = fixture.request();
    assert(compose_shared_pixel_frame(
        cold, &compositor, &output, &stats, &error));
    assert(error.kind == SharedPixelCompositorErrorKind::None);
    assert(output.pixels.size() == 64U * 32U * 4U);
    assert(output.row_bytes == 256U);
    assert(output.checksum != 0U);
    assert(stats.input_commands == 5U);
    assert(stats.input_uploads == 3U);
    assert(stats.selection_pixels == 18U * 8U);
    assert(stats.caret_pixels == 2U * 20U);
    assert(stats.alpha_glyph_pixels > 0U);
    assert(stats.lcd_glyph_pixels == 16U);
    assert(stats.color_glyph_pixels == 16U);
    assert(native_window_pixel_buffer_valid(output.view(), output.surface));

    const std::uint64_t checksum = output.checksum;
    GlyphAtlasSubmission hot_atlas;
    hot_atlas.atlas_generation_id = fixture.atlas.atlas_generation_id;
    hot_atlas.submission_epoch = fixture.atlas.submission_epoch + 1U;
    SharedPixelCompositorRequest hot = cold;
    hot.atlas_submission = &hot_atlas;
    hot.raster_payload = {};
    assert(compose_shared_pixel_frame(
        hot, &compositor, &output, &stats, &error));
    assert(stats.input_uploads == 0U);
    assert(output.checksum == checksum);
}

void test_failure_atomicity_and_stale_generation() {
    Fixture fixture;
    SharedPixelCompositor compositor = make_compositor();
    SharedCompositedFrame output;
    SharedPixelCompositorStats stats;
    SharedPixelCompositorError error;
    SharedPixelCompositorRequest request = fixture.request();
    assert(compose_shared_pixel_frame(
        request, &compositor, &output, &stats, &error));
    const std::uint64_t checksum = output.checksum;
    const std::size_t bytes = output.pixels.size();

    request.styles = std::span<const SharedPixelStyle>(
        fixture.styles.data(), 2U);
    assert(!compose_shared_pixel_frame(
        request, &compositor, &output, &stats, &error));
    assert(error.kind == SharedPixelCompositorErrorKind::InvalidStyleTable);
    assert(output.checksum == checksum);
    assert(output.pixels.size() == bytes);

    request = fixture.request();
    fixture.frame.page_references[0].page_generation += 1U;
    fixture.frame.glyph_batches[0].page_generation += 1U;
    fixture.instances[0].page_generation += 1U;
    request.draw_instances = fixture.instances;
    request.atlas_submission = nullptr;
    request.raster_payload = {};
    assert(!compose_shared_pixel_frame(
        request, &compositor, &output, &stats, &error));
    assert(error.kind ==
           SharedPixelCompositorErrorKind::StaleAtlasGeneration);
    assert(output.checksum == checksum);
}

void test_clear_invalidates_hot_pages() {
    Fixture fixture;
    SharedPixelCompositor compositor = make_compositor();
    SharedCompositedFrame output;
    SharedPixelCompositorStats stats;
    SharedPixelCompositorError error;
    SharedPixelCompositorRequest request = fixture.request();
    assert(compose_shared_pixel_frame(
        request, &compositor, &output, &stats, &error));
    assert(compositor.clear());
    GlyphAtlasSubmission hot;
    hot.atlas_generation_id = fixture.atlas.atlas_generation_id;
    hot.submission_epoch = fixture.atlas.submission_epoch + 1U;
    request.atlas_submission = &hot;
    request.raster_payload = {};
    assert(!compose_shared_pixel_frame(
        request, &compositor, &output, &stats, &error));
    assert(error.kind ==
           SharedPixelCompositorErrorKind::StaleAtlasGeneration);
}

void test_rgba_surface() {
    Fixture fixture;
    fixture.frame.surface.format = GpuSurfaceFormat::Rgba8Unorm;
    SharedPixelCompositor compositor = make_compositor();
    SharedCompositedFrame output;
    SharedPixelCompositorStats stats;
    SharedPixelCompositorError error;
    assert(compose_shared_pixel_frame(
        fixture.request(), &compositor, &output, &stats, &error));
    assert(output.pixels[0] == std::byte{0x10});
    assert(output.pixels[1] == std::byte{0x18});
    assert(output.pixels[2] == std::byte{0x20});
    assert(output.pixels[3] == std::byte{0xFF});
}

} // namespace

int main() {
    for (int repetition = 0; repetition < 20; ++repetition) {
        test_cold_and_hot_composition();
        test_failure_atomicity_and_stale_generation();
        test_clear_invalidates_hot_pages();
        test_rgba_surface();
    }
    return 0;
}
