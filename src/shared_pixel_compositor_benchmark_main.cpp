#include "shared_pixel_compositor.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

using namespace zevryon::text;

std::uint64_t percentile(
    const std::vector<std::uint64_t>& sorted,
    std::uint32_t permille) {
    if (sorted.empty()) {
        return 0U;
    }
    const std::size_t index = std::min<std::size_t>(
        sorted.size() - 1U,
        (static_cast<std::uint64_t>(sorted.size() - 1U) * permille + 999U) /
            1000U);
    return sorted[index];
}

double to_ms(std::uint64_t nanoseconds) {
    return static_cast<double>(nanoseconds) / 1'000'000.0;
}

struct Scene final {
    GpuFrameSubmission frame;
    GlyphAtlasSubmission cold_atlas;
    GlyphAtlasSubmission hot_atlas;
    std::vector<GlyphAtlasDrawInstance> instances;
    std::vector<std::byte> payload;
    std::array<SharedPixelStyle, 3U> styles{{
        {1U, 0x3060C060U},
        {2U, 0xE8EDF4FFU},
        {3U, 0xFF6040FFU}}};

    Scene() {
        frame.surface.surface_id = 100U;
        frame.surface.generation_id = 200U;
        frame.surface.width = 640U;
        frame.surface.height = 360U;
        frame.surface.format = GpuSurfaceFormat::Bgra8Unorm;
        frame.surface.premultiplied_alpha = 1U;
        frame.frame_id = 300U;
        frame.atlas_generation_id = 400U;
        frame.atlas_submission_epoch = 500U;
        frame.clips.push_back(TextPaintClipRect{0, 0, 640U, 360U});

        for (std::uint32_t index = 0U; index < 64U; ++index) {
            const std::int64_t x =
                8 + static_cast<std::int64_t>((index % 8U) * 78U);
            const std::int64_t y =
                8 + static_cast<std::int64_t>((index / 8U) * 42U);
            frame.fill_rects.push_back(TextPaintFillRect{
                x, y, 68U, 20U, 1U, index, index,
                kTextPaintRectSelection});
            frame.commands.push_back(GpuFrameCommandRecord{
                GpuFrameCommandKind::FillRect, index, 0U, 0U});
        }

        for (std::uint32_t page = 0U; page < 3U; ++page) {
            frame.page_references.push_back(GpuFramePageReference{
                700U + page,
                0U,
                page,
                page,
                1U,
                page == 0U ? GlyphRasterFormat::Alpha8 :
                page == 1U ? GlyphRasterFormat::LcdRgb8 :
                             GlyphRasterFormat::Bgra8,
                0U,
                0U});
            const std::uint32_t first =
                static_cast<std::uint32_t>(instances.size());
            for (std::uint32_t instance_index = 0U;
                 instance_index < 80U; ++instance_index) {
                GlyphAtlasDrawInstance instance;
                instance.viewport_inline_start =
                    12 + static_cast<std::int64_t>(
                        (instance_index % 20U) * 30U);
                instance.viewport_block_start =
                    18 + static_cast<std::int64_t>(
                        page * 100U + (instance_index / 20U) * 20U);
                instance.atlas_generation_id = 400U;
                instance.page_generation = 700U + page;
                instance.page_index = page;
                instance.atlas_x = (instance_index % 8U) * 8U;
                instance.atlas_y = ((instance_index / 8U) % 8U) * 8U;
                instance.width = 8U;
                instance.height = 8U;
                instance.style_id = 2U;
                instance.clip_index = 0U;
                instance.working_set_key_index = instance_index;
                instances.push_back(instance);
            }
            frame.glyph_batches.push_back(GpuFrameGlyphBatch{
                700U + page,
                page,
                first,
                80U,
                2U,
                0U,
                page,
                0U});
            frame.commands.push_back(GpuFrameCommandRecord{
                GpuFrameCommandKind::GlyphBatch, page, 0U, 0U});
        }

        frame.fill_rects.push_back(TextPaintFillRect{
            318, 20, 2U, 320U, 3U, 0U, 0U, kTextPaintRectCaret});
        frame.commands.push_back(GpuFrameCommandRecord{
            GpuFrameCommandKind::FillRect, 64U, 0U, 0U});

        cold_atlas.atlas_generation_id = 400U;
        cold_atlas.submission_epoch = 500U;
        hot_atlas.atlas_generation_id = 400U;
        hot_atlas.submission_epoch = 501U;

        const auto append_page = [this](
            std::uint32_t page,
            GlyphRasterFormat format,
            std::uint32_t channels) {
            const std::uint64_t offset = payload.size();
            const std::uint32_t row_bytes = 64U * channels;
            const std::uint64_t bytes =
                static_cast<std::uint64_t>(row_bytes) * 64U;
            payload.resize(payload.size() + static_cast<std::size_t>(bytes));
            for (std::uint32_t y = 0U; y < 64U; ++y) {
                for (std::uint32_t x = 0U; x < 64U; ++x) {
                    const std::size_t base =
                        static_cast<std::size_t>(offset) +
                        static_cast<std::size_t>(y) * row_bytes +
                        static_cast<std::size_t>(x) * channels;
                    if (format == GlyphRasterFormat::Alpha8) {
                        payload[base] = static_cast<std::byte>(
                            32U + ((x * 3U + y * 5U) % 224U));
                    } else if (format == GlyphRasterFormat::LcdRgb8) {
                        payload[base + 0U] = static_cast<std::byte>(
                            64U + ((x * 7U) % 192U));
                        payload[base + 1U] = static_cast<std::byte>(
                            64U + ((y * 9U) % 192U));
                        payload[base + 2U] = static_cast<std::byte>(
                            64U + (((x + y) * 11U) % 192U));
                    } else {
                        payload[base + 0U] =
                            static_cast<std::byte>(32U + (x * 3U) % 224U);
                        payload[base + 1U] =
                            static_cast<std::byte>(48U + (y * 5U) % 208U);
                        payload[base + 2U] =
                            static_cast<std::byte>(
                                64U + ((x + y) * 7U) % 192U);
                        payload[base + 3U] =
                            static_cast<std::byte>(96U + (x * y) % 160U);
                    }
                }
            }
            cold_atlas.uploads.push_back(GlyphAtlasUploadRecord{
                400U,
                700U + page,
                offset,
                bytes,
                page,
                0U,
                0U,
                64U,
                64U,
                row_bytes,
                page,
                format,
                0U,
                0U});
        };
        append_page(0U, GlyphRasterFormat::Alpha8, 1U);
        append_page(1U, GlyphRasterFormat::LcdRgb8, 3U);
        append_page(2U, GlyphRasterFormat::Bgra8, 4U);
    }

    SharedPixelCompositorRequest request(
        const GlyphAtlasSubmission* atlas) const {
        SharedPixelCompositorRequest request;
        request.frame = &frame;
        request.atlas_submission = atlas;
        request.draw_instances = instances;
        request.raster_payload =
            atlas == &cold_atlas
            ? std::span<const std::byte>(payload)
            : std::span<const std::byte>();
        request.styles = styles;
        request.background_rgba8 = 0x101820FFU;
        request.limits.maximum_commands = 68U;
        request.limits.maximum_fill_rects = 65U;
        request.limits.maximum_glyph_batches = 3U;
        request.limits.maximum_draw_instances = 240U;
        request.limits.maximum_upload_bytes = payload.size();
        return request;
    }
};

} // namespace

int main(int argc, char** argv) {
    using namespace zevryon::text;
    const std::uint32_t iterations =
        argc > 1
        ? static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10))
        : 128U;
    assert(iterations > 0U);

    Scene scene;
    SharedPixelCompositor compositor(
        SharedPixelCompositorConfig{
            64U,
            64U,
            3U,
            16U,
            640U * 360U * 4U,
            64U * 64U * 4U * 3U});
    SharedCompositedFrame output;
    SharedPixelCompositorStats stats;
    SharedPixelCompositorError error;

    assert(compose_shared_pixel_frame(
        scene.request(&scene.cold_atlas),
        &compositor,
        &output,
        &stats,
        &error));
    const std::uint64_t expected_checksum = output.checksum;
    const std::uint64_t alpha_pixels = stats.alpha_glyph_pixels;
    const std::uint64_t lcd_pixels = stats.lcd_glyph_pixels;
    const std::uint64_t color_pixels = stats.color_glyph_pixels;
    const std::uint64_t selection_pixels = stats.selection_pixels;
    const std::uint64_t caret_pixels = stats.caret_pixels;

    std::vector<std::uint64_t> samples;
    samples.reserve(iterations);
    for (std::uint32_t iteration = 0U;
         iteration < iterations; ++iteration) {
        const auto begin = std::chrono::steady_clock::now();
        assert(compose_shared_pixel_frame(
            scene.request(&scene.hot_atlas),
            &compositor,
            &output,
            &stats,
            &error));
        const auto end = std::chrono::steady_clock::now();
        assert(output.checksum == expected_checksum);
        samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - begin).count()));
    }

    std::sort(samples.begin(), samples.end());
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "iterations=" << iterations << '\n';
    std::cout << "samples=" << samples.size() << '\n';
    std::cout << "commands=" << scene.frame.commands.size() << '\n';
    std::cout << "fill_rects=" << scene.frame.fill_rects.size() << '\n';
    std::cout << "glyph_batches=" << scene.frame.glyph_batches.size() << '\n';
    std::cout << "draw_instances=" << scene.instances.size() << '\n';
    std::cout << "cold_uploads=" << scene.cold_atlas.uploads.size() << '\n';
    std::cout << "payload_bytes=" << scene.payload.size() << '\n';
    std::cout << "surface_bytes=" << output.pixels.size() << '\n';
    std::cout << "atlas_bytes=" << compositor.atlas_bytes() << '\n';
    std::cout << "selection_pixels=" << selection_pixels << '\n';
    std::cout << "caret_pixels=" << caret_pixels << '\n';
    std::cout << "alpha_glyph_pixels=" << alpha_pixels << '\n';
    std::cout << "lcd_glyph_pixels=" << lcd_pixels << '\n';
    std::cout << "color_glyph_pixels=" << color_pixels << '\n';
    std::cout << "checksum=" << expected_checksum << '\n';
    std::cout << "p50_ms=" << to_ms(percentile(samples, 500U)) << '\n';
    std::cout << "p95_ms=" << to_ms(percentile(samples, 950U)) << '\n';
    std::cout << "p99_ms=" << to_ms(percentile(samples, 990U)) << '\n';
    std::cout << "max_ms=" << to_ms(samples.back()) << '\n';
    return 0;
}
