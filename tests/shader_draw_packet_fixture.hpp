#pragma once

#include "shader_draw_packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace zevryon::text::test {

struct ShaderPacketFixture final {
    ShaderSurface surface{640U, 360U,
        kShaderPacketPremultipliedAlpha |
        kShaderPacketTopLeftOrigin |
        kShaderPacketNearestAtlasSampling,
        0U};
    ShaderPacketLimits limits{
        128U, 16U, 128U, 512U, 8U, 8U, 2048U, 2048U,
        1U << 20U, 1U << 20U, 1U << 20U, 0U};
    std::vector<ShaderSourceCommand> commands;
    std::vector<ShaderFillSource> fills;
    std::vector<ShaderGlyphSource> glyphs;
    std::vector<ShaderAtlasUploadSource> uploads;
    std::vector<std::byte> payload;

    ShaderPacketInput input(
        std::uint64_t frame_id = 1U,
        std::uint64_t atlas_generation = 7U) const noexcept {
        return ShaderPacketInput{
            surface,
            limits,
            frame_id,
            atlas_generation,
            commands,
            fills,
            glyphs,
            uploads,
            payload};
    }
};

inline ShaderPacketFixture make_shader_packet_fixture() {
    ShaderPacketFixture fixture;
    const ShaderRectI full_clip{0, 0, 640, 360};

    fixture.fills.reserve(65U);
    fixture.commands.reserve(68U);
    for (std::uint32_t index = 0U; index < 64U; ++index) {
        const std::int32_t column = static_cast<std::int32_t>(index % 8U);
        const std::int32_t row = static_cast<std::int32_t>(index / 8U);
        fixture.fills.push_back(ShaderFillSource{
            ShaderRectI{16 + column * 72, 12 + row * 7, 56, 5},
            full_clip,
            ShaderColorBgra8{64U, 32U, 16U, 128U},
            ShaderLayer::Selection,
            {0U, 0U, 0U},
            1000U + index});
        fixture.commands.push_back(ShaderSourceCommand{
            ShaderPrimitiveKind::Fill,
            ShaderLayer::Selection,
            {0U, 0U},
            index,
            1U,
            2000U + index});
    }

    fixture.payload.resize(32'768U);
    std::size_t cursor = 0U;
    for (std::uint32_t y = 0U; y < 64U; ++y) {
        for (std::uint32_t x = 0U; x < 64U; ++x) {
            fixture.payload[cursor++] = static_cast<std::byte>(
                static_cast<std::uint8_t>((x * 3U + y * 5U + 17U) & 0xFFU));
        }
    }
    for (std::uint32_t y = 0U; y < 64U; ++y) {
        for (std::uint32_t x = 0U; x < 64U; ++x) {
            fixture.payload[cursor++] = static_cast<std::byte>(
                static_cast<std::uint8_t>((x * 7U + y * 3U + 11U) & 0xFFU));
            fixture.payload[cursor++] = static_cast<std::byte>(
                static_cast<std::uint8_t>((x * 5U + y * 9U + 29U) & 0xFFU));
            fixture.payload[cursor++] = static_cast<std::byte>(
                static_cast<std::uint8_t>((x * 13U + y * 2U + 47U) & 0xFFU));
        }
    }
    for (std::uint32_t y = 0U; y < 64U; ++y) {
        for (std::uint32_t x = 0U; x < 64U; ++x) {
            const std::uint8_t alpha = static_cast<std::uint8_t>(
                96U + ((x + y) % 160U));
            fixture.payload[cursor++] = static_cast<std::byte>(alpha / 4U);
            fixture.payload[cursor++] = static_cast<std::byte>(alpha / 3U);
            fixture.payload[cursor++] = static_cast<std::byte>(alpha / 2U);
            fixture.payload[cursor++] = static_cast<std::byte>(alpha);
        }
    }

    const std::array<std::uint64_t, 3U> offsets{0U, 4096U, 16'384U};
    const std::array<std::uint64_t, 3U> sizes{4096U, 12'288U, 16'384U};
    const std::array<std::uint32_t, 3U> rows{64U, 192U, 256U};
    const std::array<ShaderAtlasFormat, 3U> formats{
        ShaderAtlasFormat::Alpha8,
        ShaderAtlasFormat::LcdRgb8,
        ShaderAtlasFormat::Bgra8};
    for (std::uint32_t page = 0U; page < 3U; ++page) {
        const auto bytes = std::span<const std::byte>(fixture.payload).subspan(
            static_cast<std::size_t>(offsets[page]),
            static_cast<std::size_t>(sizes[page]));
        fixture.uploads.push_back(ShaderAtlasUploadSource{
            page,
            3U + page,
            64U,
            64U,
            rows[page],
            formats[page],
            {0U, 0U, 0U},
            offsets[page],
            sizes[page],
            shader_bytes_checksum(bytes)});
    }

    fixture.glyphs.reserve(240U);
    for (std::uint32_t batch = 0U; batch < 3U; ++batch) {
        const std::uint32_t first = static_cast<std::uint32_t>(fixture.glyphs.size());
        for (std::uint32_t index = 0U; index < 80U; ++index) {
            const std::int32_t column = static_cast<std::int32_t>(index % 20U);
            const std::int32_t row = static_cast<std::int32_t>(index / 20U);
            fixture.glyphs.push_back(ShaderGlyphSource{
                ShaderRectI{
                    24 + column * 28,
                    80 + static_cast<std::int32_t>(batch) * 82 + row * 18,
                    12,
                    16},
                full_clip,
                batch,
                3U + batch,
                static_cast<std::uint16_t>((index % 8U) * 8U),
                static_cast<std::uint16_t>(((index / 8U) % 4U) * 16U),
                8U,
                16U,
                ShaderColorBgra8{
                    static_cast<std::uint8_t>(220U - batch * 20U),
                    static_cast<std::uint8_t>(200U - batch * 10U),
                    240U,
                    255U},
                formats[batch],
                ShaderLayer::Glyph,
                {0U, 0U},
                3000U + first + index});
        }
        fixture.commands.push_back(ShaderSourceCommand{
            ShaderPrimitiveKind::GlyphBatch,
            ShaderLayer::Glyph,
            {0U, 0U},
            first,
            80U,
            4000U + batch});
    }

    fixture.fills.push_back(ShaderFillSource{
        ShaderRectI{608, 72, 2, 80},
        full_clip,
        ShaderColorBgra8{255U, 255U, 255U, 255U},
        ShaderLayer::Caret,
        {0U, 0U, 0U},
        5000U});
    fixture.commands.push_back(ShaderSourceCommand{
        ShaderPrimitiveKind::Fill,
        ShaderLayer::Caret,
        {0U, 0U},
        64U,
        1U,
        5001U});

    return fixture;
}

} // namespace zevryon::text::test
