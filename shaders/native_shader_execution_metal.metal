#include <metal_stdlib>
using namespace metal;

struct FillRecord {
    int x;
    int y;
    int width;
    int height;
    uint color;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

struct GlyphRecord {
    int x;
    int y;
    int width;
    int height;
    uint atlasSlice;
    uint atlasX;
    uint atlasY;
    uint atlasWidth;
    uint atlasHeight;
    uint color;
    uint format;
    uint reserved0;
    uint reserved1;
    uint reserved2;
    uint reserved3;
    uint reserved4;
};

struct DispatchConstants {
    uint surfaceWidth;
    uint surfaceHeight;
    uint operation;
    uint instanceIndex;
    uint dispatchOriginX;
    uint dispatchOriginY;
    uint dispatchWidth;
    uint dispatchHeight;
};

uint channel_b(uint value) { return value & 255U; }
uint channel_g(uint value) { return (value >> 8U) & 255U; }
uint channel_r(uint value) { return (value >> 16U) & 255U; }
uint channel_a(uint value) { return (value >> 24U) & 255U; }

uint pack_bgra(uint b, uint g, uint r, uint a) {
    return b | (g << 8U) | (r << 16U) | (a << 24U);
}

uint mul_u8(uint left, uint right) {
    return (left * right + 127U) / 255U;
}

uint blend_channel(uint source, uint destination, uint sourceAlpha) {
    uint inverse = 255U - sourceAlpha;
    uint value = source * 255U + destination * inverse + 127U;
    return min(255U, value / 255U);
}

uint blend_pixel(uint destination, uint source) {
    uint alpha = channel_a(source);
    return pack_bgra(
        blend_channel(channel_b(source), channel_b(destination), alpha),
        blend_channel(channel_g(source), channel_g(destination), alpha),
        blend_channel(channel_r(source), channel_r(destination), alpha),
        blend_channel(alpha, channel_a(destination), alpha));
}

bool inside_rect(int2 pixel, int x, int y, int width, int height) {
    return pixel.x >= x && pixel.y >= y &&
           pixel.x < x + width && pixel.y < y + height;
}

uint premultiply_coverage(uint color, uint coverage) {
    uint alpha = mul_u8(channel_a(color), coverage);
    return pack_bgra(
        mul_u8(channel_b(color), alpha),
        mul_u8(channel_g(color), alpha),
        mul_u8(channel_r(color), alpha),
        alpha);
}

kernel void zevryon_integer_composer(
    device const FillRecord* fills [[buffer(0)]],
    device const GlyphRecord* glyphs [[buffer(1)]],
    device uint* output [[buffer(2)]],
    constant DispatchConstants& constants [[buffer(3)]],
    texture2d_array<uint, access::read> atlas [[texture(0)]],
    uint2 dispatchId [[thread_position_in_grid]]) {
    if (constants.operation == 0U) {
        if (dispatchId.x < constants.surfaceWidth &&
            dispatchId.y < constants.surfaceHeight) {
            output[dispatchId.y * constants.surfaceWidth + dispatchId.x] = 0U;
        }
        return;
    }

    if (dispatchId.x >= constants.dispatchWidth ||
        dispatchId.y >= constants.dispatchHeight) {
        return;
    }

    uint2 outputPixel = uint2(
        constants.dispatchOriginX + dispatchId.x,
        constants.dispatchOriginY + dispatchId.y);
    if (outputPixel.x >= constants.surfaceWidth ||
        outputPixel.y >= constants.surfaceHeight) {
        return;
    }

    int2 pixel = int2(outputPixel);
    uint outputIndex = outputPixel.y * constants.surfaceWidth + outputPixel.x;
    uint composed = output[outputIndex];

    if (constants.operation == 1U) {
        FillRecord fill = fills[constants.instanceIndex];
        if (inside_rect(pixel, fill.x, fill.y, fill.width, fill.height)) {
            composed = blend_pixel(composed, fill.color);
        }
    } else if (constants.operation == 2U) {
        GlyphRecord glyph = glyphs[constants.instanceIndex];
        if (inside_rect(pixel, glyph.x, glyph.y, glyph.width, glyph.height)) {
            uint localX = uint(pixel.x - glyph.x);
            uint localY = uint(pixel.y - glyph.y);
            uint sourceX = glyph.atlasX +
                (localX * glyph.atlasWidth) / uint(glyph.width);
            uint sourceY = glyph.atlasY +
                (localY * glyph.atlasHeight) / uint(glyph.height);
            uint texel = atlas.read(
                uint2(sourceX, sourceY), glyph.atlasSlice).x;
            uint source = 0U;
            if (glyph.format == 0U) {
                source = premultiply_coverage(glyph.color, channel_a(texel));
            } else if (glyph.format == 1U) {
                uint coverageB = channel_b(texel);
                uint coverageG = channel_g(texel);
                uint coverageR = channel_r(texel);
                uint modulationAlpha = channel_a(glyph.color);
                uint alpha = mul_u8(
                    modulationAlpha,
                    max(coverageB, max(coverageG, coverageR)));
                source = pack_bgra(
                    mul_u8(mul_u8(channel_b(glyph.color), modulationAlpha), coverageB),
                    mul_u8(mul_u8(channel_g(glyph.color), modulationAlpha), coverageG),
                    mul_u8(mul_u8(channel_r(glyph.color), modulationAlpha), coverageR),
                    alpha);
            } else {
                uint modulationAlpha = channel_a(glyph.color);
                source = pack_bgra(
                    mul_u8(channel_b(texel), modulationAlpha),
                    mul_u8(channel_g(texel), modulationAlpha),
                    mul_u8(channel_r(texel), modulationAlpha),
                    mul_u8(channel_a(texel), modulationAlpha));
            }
            composed = blend_pixel(composed, source);
        }
    }

    output[outputIndex] = composed;
}
