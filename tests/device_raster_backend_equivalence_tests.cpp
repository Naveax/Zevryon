#include "device_raster_backend.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {
using namespace zevryon::text;

std::uint64_t fnv1a64(std::span<const std::byte> bytes) {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (std::byte value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

std::vector<std::byte> oracle_render(
    const DeviceGlyphRasterJob& job,
    std::uint64_t resource_id,
    const DeviceGlyphRasterMetrics& metrics) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(metrics.payload_size));
    std::uint64_t state = resource_id ^ job.key.font_generation_id ^
        (static_cast<std::uint64_t>(job.key.glyph_id) << 17U) ^
        (static_cast<std::uint64_t>(job.key.subpixel_x) << 9U) ^
        (static_cast<std::uint64_t>(job.key.subpixel_y) << 1U) ^
        static_cast<std::uint64_t>(job.key.mode);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        std::uint8_t value = static_cast<std::uint8_t>((state + i * 31U) & 0xFFU);
        if (metrics.format == GlyphRasterFormat::Bgra8 && (i % 4U) == 3U) {
            value = 0xFFU;
        }
        bytes[i] = static_cast<std::byte>(value);
    }
    return bytes;
}

} // namespace

int main() {
    using namespace zevryon::text;
    ReferenceDeviceGlyphRasterBackend backend;
    const std::array<std::int32_t, 4> scales{512, 1'024, 2'048, 4'096};
    const std::array<std::uint64_t, 3> resources{11U, 97U, 1'001U};
    const std::array<GlyphRasterMode, 3> modes{
        GlyphRasterMode::Grayscale, GlyphRasterMode::Lcd, GlyphRasterMode::Color};
    std::array<std::byte, 128> face_bytes{};
    std::uint64_t passed = 0U;
    std::uint64_t total = 0U;

    for (std::uint64_t resource_id : resources) {
        for (GlyphRasterMode mode : modes) {
            for (std::int32_t scale : scales) {
                for (std::uint8_t phase_grid = 1U; phase_grid <= 4U; ++phase_grid) {
                    for (std::uint32_t glyph_id = 1U; glyph_id <= 56U; ++glyph_id) {
                        ++total;
                        DeviceRasterPolicy policy;
                        policy.grayscale_phase_count = phase_grid;
                        policy.lcd_phase_count = phase_grid;
                        DeviceRasterFaceSource face;
                        face.font_generation_id = 7U;
                        face.face_id = 3U;
                        face.resource_id = resource_id;
                        face.bytes = face_bytes;
                        DeviceGlyphRasterJob job;
                        job.key.font_generation_id = 7U;
                        job.key.face_id = 3U;
                        job.key.glyph_id = glyph_id;
                        job.key.x_scale = scale;
                        job.key.y_scale = scale;
                        job.key.mode = mode;
                        job.key.subpixel_x = mode == GlyphRasterMode::Color ? 0U :
                            static_cast<std::uint8_t>((glyph_id - 1U) % phase_grid);
                        job.key.subpixel_y = job.key.subpixel_x;
                        job.queue_generation = 1U;
                        job.job_id = glyph_id;
                        job.face_resource_id = resource_id;
                        job.device_x_scale = scale;
                        job.device_y_scale = scale;

                        DeviceGlyphRasterMetrics actual;
                        DeviceGlyphRasterBackendError error;
                        assert(backend.query(job, face, policy, &actual, &error));
                        const bool empty = glyph_id % 29U == 0U;
                        if (empty) {
                            assert(actual.format == GlyphRasterFormat::Empty);
                            assert(actual.payload_size == 0U);
                            ++passed;
                            continue;
                        }
                        const std::uint32_t base = std::max<std::uint32_t>(1U,
                            static_cast<std::uint32_t>(scale / 64));
                        const std::uint32_t expected_width =
                            1U + glyph_id % base;
                        const std::uint32_t expected_height =
                            1U + ((glyph_id * 7U + 3U) % base);
                        const std::uint32_t bpp = mode == GlyphRasterMode::Grayscale ? 1U :
                            (mode == GlyphRasterMode::Lcd ? 3U : 4U);
                        assert(actual.width == expected_width);
                        assert(actual.height == expected_height);
                        assert(actual.row_bytes == expected_width * bpp);
                        assert(actual.payload_size ==
                            static_cast<std::uint64_t>(actual.row_bytes) * expected_height);
                        const GlyphRasterFormat expected_format =
                            mode == GlyphRasterMode::Grayscale ? GlyphRasterFormat::Alpha8 :
                            (mode == GlyphRasterMode::Lcd ? GlyphRasterFormat::LcdRgb8 :
                                GlyphRasterFormat::Bgra8);
                        assert(actual.format == expected_format);
                        std::vector<std::byte> rendered(
                            static_cast<std::size_t>(actual.payload_size));
                        assert(backend.render(job, face, policy, actual, rendered, &error));
                        const std::vector<std::byte> expected =
                            oracle_render(job, resource_id, actual);
                        assert(rendered == expected);
                        assert(fnv1a64(rendered) == fnv1a64(expected));
                        ++passed;
                    }
                }
            }
        }
    }
    assert(total == 8'064U);
    assert(passed == total);
    std::cout << "device-raster-backend-equivalence: " << passed << "/" << total
              << " PASS\n";
    return 0;
}
