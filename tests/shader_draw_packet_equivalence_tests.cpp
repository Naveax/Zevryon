#include "shader_draw_packet.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <span>
#include <vector>

namespace {
using namespace zevryon::text;

std::uint8_t multiply(std::uint8_t left, std::uint8_t right) {
    return static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(left) * right + 127U) / 255U);
}

void oracle_blend(
    std::vector<std::byte>* pixels,
    std::uint32_t row_bytes,
    std::int32_t x,
    std::int32_t y,
    ShaderColorBgra8 source) {
    auto* destination = reinterpret_cast<std::uint8_t*>(
        pixels->data() + static_cast<std::size_t>(y) * row_bytes +
        static_cast<std::size_t>(x) * 4U);
    const std::uint32_t inverse = 255U - source.alpha;
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const std::uint8_t source_channel =
            channel == 0U ? source.blue :
            channel == 1U ? source.green : source.red;
        destination[channel] = static_cast<std::uint8_t>(std::min(
            255U,
            (static_cast<std::uint32_t>(source_channel) * 255U +
             static_cast<std::uint32_t>(destination[channel]) * inverse + 127U) /
                255U));
    }
    destination[3] = static_cast<std::uint8_t>(std::min(
        255U,
        (static_cast<std::uint32_t>(source.alpha) * 255U +
         static_cast<std::uint32_t>(destination[3]) * inverse + 127U) /
            255U));
}

ShaderRectI clip_rect(const ShaderRectI& a, const ShaderRectI& b) {
    const std::int32_t x0 = std::max(a.x, b.x);
    const std::int32_t y0 = std::max(a.y, b.y);
    const std::int32_t x1 = std::min(a.x + a.width, b.x + b.width);
    const std::int32_t y1 = std::min(a.y + a.height, b.y + b.height);
    return x1 <= x0 || y1 <= y0 ? ShaderRectI{} :
        ShaderRectI{x0, y0, x1 - x0, y1 - y0};
}

void oracle_fill(
    std::vector<std::byte>* pixels,
    std::uint32_t row_bytes,
    const ShaderFillSource& fill) {
    const ShaderRectI draw = clip_rect(fill.destination, fill.clip);
    for (std::int32_t y = draw.y; y < draw.y + draw.height; ++y) {
        for (std::int32_t x = draw.x; x < draw.x + draw.width; ++x) {
            oracle_blend(pixels, row_bytes, x, y, fill.color);
        }
    }
}

ShaderColorBgra8 oracle_glyph_color(
    ShaderAtlasFormat format,
    ShaderColorBgra8 color,
    const std::uint8_t* sample) {
    if (format == ShaderAtlasFormat::Alpha8) {
        const std::uint8_t alpha = multiply(color.alpha, sample[0]);
        return ShaderColorBgra8{
            multiply(color.blue, alpha),
            multiply(color.green, alpha),
            multiply(color.red, alpha),
            alpha};
    }
    if (format == ShaderAtlasFormat::LcdRgb8) {
        const std::uint8_t alpha = multiply(
            color.alpha, std::max({sample[0], sample[1], sample[2]}));
        return ShaderColorBgra8{
            multiply(multiply(color.blue, color.alpha), sample[2]),
            multiply(multiply(color.green, color.alpha), sample[1]),
            multiply(multiply(color.red, color.alpha), sample[0]),
            alpha};
    }
    return ShaderColorBgra8{
        multiply(sample[0], color.alpha),
        multiply(sample[1], color.alpha),
        multiply(sample[2], color.alpha),
        multiply(sample[3], color.alpha)};
}

void oracle_glyph(
    std::vector<std::byte>* pixels,
    std::uint32_t row_bytes,
    const ShaderGlyphSource& glyph,
    std::span<const std::byte> payload,
    std::uint32_t payload_row_bytes) {
    const ShaderRectI draw = clip_rect(glyph.destination, glyph.clip);
    const std::uint32_t texel_bytes =
        glyph.format == ShaderAtlasFormat::Alpha8 ? 1U :
        glyph.format == ShaderAtlasFormat::LcdRgb8 ? 3U : 4U;
    for (std::int32_t y = draw.y; y < draw.y + draw.height; ++y) {
        const std::uint32_t local_y = static_cast<std::uint32_t>(
            y - glyph.destination.y);
        const std::uint32_t source_y = glyph.atlas_y +
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(local_y) * glyph.atlas_height) /
                static_cast<std::uint32_t>(glyph.destination.height));
        for (std::int32_t x = draw.x; x < draw.x + draw.width; ++x) {
            const std::uint32_t local_x = static_cast<std::uint32_t>(
                x - glyph.destination.x);
            const std::uint32_t source_x = glyph.atlas_x +
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(local_x) * glyph.atlas_width) /
                    static_cast<std::uint32_t>(glyph.destination.width));
            const std::size_t offset =
                static_cast<std::size_t>(source_y) * payload_row_bytes +
                static_cast<std::size_t>(source_x) * texel_bytes;
            const auto* sample = reinterpret_cast<const std::uint8_t*>(
                payload.data() + offset);
            oracle_blend(
                pixels, row_bytes, x, y,
                oracle_glyph_color(glyph.format, glyph.color, sample));
        }
    }
}

} // namespace

int main() {
    using namespace zevryon::text;
    constexpr std::uint32_t kCases = 4096U;
    for (std::uint32_t case_index = 0U; case_index < kCases; ++case_index) {
        const ShaderAtlasFormat format = static_cast<ShaderAtlasFormat>(
            case_index % 3U);
        const std::uint32_t texel_bytes =
            format == ShaderAtlasFormat::Alpha8 ? 1U :
            format == ShaderAtlasFormat::LcdRgb8 ? 3U : 4U;
        std::vector<std::byte> payload(4U * 4U * texel_bytes);
        for (std::size_t index = 0U; index < payload.size(); ++index) {
            payload[index] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    (case_index * 17U + static_cast<std::uint32_t>(index) * 29U + 3U) &
                    0xFFU));
        }
        if (format == ShaderAtlasFormat::Bgra8) {
            for (std::size_t pixel = 0U; pixel < 16U; ++pixel) {
                auto* bytes = reinterpret_cast<std::uint8_t*>(
                    payload.data() + pixel * 4U);
                const std::uint8_t alpha = bytes[3];
                bytes[0] = multiply(bytes[0], alpha);
                bytes[1] = multiply(bytes[1], alpha);
                bytes[2] = multiply(bytes[2], alpha);
            }
        }

        const ShaderRectI clip{
            static_cast<std::int32_t>((case_index >> 2U) % 3U),
            static_cast<std::int32_t>((case_index >> 4U) % 3U),
            14,
            14};
        std::array<ShaderFillSource, 2U> fills{{
            ShaderFillSource{
                ShaderRectI{1, 1, 11, 5},
                clip,
                ShaderColorBgra8{32U, 48U, 64U, 128U},
                ShaderLayer::Selection,
                {0U, 0U, 0U},
                1U},
            ShaderFillSource{
                ShaderRectI{13, 2, 1, 11},
                clip,
                ShaderColorBgra8{255U, 255U, 255U, 255U},
                ShaderLayer::Caret,
                {0U, 0U, 0U},
                3U}}};
        std::array<ShaderGlyphSource, 1U> glyphs{{
            ShaderGlyphSource{
                ShaderRectI{
                    static_cast<std::int32_t>(2U + case_index % 5U),
                    static_cast<std::int32_t>(6U + (case_index >> 3U) % 3U),
                    7,
                    6},
                clip,
                0U,
                9U,
                0U,
                0U,
                4U,
                4U,
                ShaderColorBgra8{220U, 210U, 240U, 255U},
                format,
                ShaderLayer::Glyph,
                {0U, 0U},
                2U}}};
        std::array<ShaderSourceCommand, 3U> commands{{
            ShaderSourceCommand{
                ShaderPrimitiveKind::Fill, ShaderLayer::Selection,
                {0U, 0U}, 0U, 1U, 10U},
            ShaderSourceCommand{
                ShaderPrimitiveKind::GlyphBatch, ShaderLayer::Glyph,
                {0U, 0U}, 0U, 1U, 11U},
            ShaderSourceCommand{
                ShaderPrimitiveKind::Fill, ShaderLayer::Caret,
                {0U, 0U}, 1U, 1U, 12U}}};
        std::array<ShaderAtlasUploadSource, 1U> uploads{{
            ShaderAtlasUploadSource{
                0U,
                9U,
                4U,
                4U,
                4U * texel_bytes,
                format,
                {0U, 0U, 0U},
                0U,
                payload.size(),
                shader_bytes_checksum(payload)}}};
        const ShaderPacketLimits limits{
            8U, 8U, 8U, 8U, 4U, 4U, 16U, 16U,
            4096U, 4096U, 4096U, 0U};
        const ShaderPacketInput input{
            ShaderSurface{16U, 16U,
                kShaderPacketPremultipliedAlpha |
                kShaderPacketTopLeftOrigin |
                kShaderPacketNearestAtlasSampling,
                0U},
            limits,
            1U + case_index,
            2U,
            commands,
            fills,
            glyphs,
            uploads,
            payload};

        std::array<std::byte, 8192U> storage{};
        std::pmr::monotonic_buffer_resource resource(storage.data(), storage.size());
        GpuShaderPacket packet(&resource);
        ShaderPacketError error;
        assert(compile_gpu_shader_packet(input, &packet, &error));
        ShaderAtlasResidency atlas(4U, 4096U);
        assert(atlas.apply_packet_uploads(packet, &error));
        ShaderReadback readback;
        assert(execute_shader_packet_reference(packet, atlas, &readback, &error));

        std::vector<std::byte> expected(16U * 16U * 4U, std::byte{0});
        oracle_fill(&expected, 64U, fills[0]);
        oracle_glyph(&expected, 64U, glyphs[0], payload, 4U * texel_bytes);
        oracle_fill(&expected, 64U, fills[1]);
        assert(readback.bgra == expected);
        assert(readback.checksum == shader_bytes_checksum(expected));
    }
    std::cout << "shader draw packet equivalence: 4096/4096 PASS\n";
    return 0;
}
