#include <metal_stdlib>
using namespace metal;

struct PushConstants {
    uint4 a;
    uint4 b;
    uint4 c;
    uint4 d;
    uint4 e;
    uint4 f;
};

inline uint channel_b(uint value) { return value & 255U; }
inline uint channel_g(uint value) { return (value >> 8U) & 255U; }
inline uint channel_r(uint value) { return (value >> 16U) & 255U; }
inline uint channel_a(uint value) { return (value >> 24U) & 255U; }

inline uint pack_bgra(uint b, uint g, uint r, uint a) {
    return b | (g << 8U) | (r << 16U) | (a << 24U);
}

inline uint mul_u8(uint left, uint right) {
    return (left * right + 127U) / 255U;
}

inline uint blend_channel(uint source, uint destination, uint source_alpha) {
    uint inverse = 255U - source_alpha;
    uint value = source * 255U + destination * inverse + 127U;
    return min(255U, value / 255U);
}

inline uint blend_pixel(uint destination, uint source) {
    uint alpha = channel_a(source);
    return pack_bgra(
        blend_channel(channel_b(source), channel_b(destination), alpha),
        blend_channel(channel_g(source), channel_g(destination), alpha),
        blend_channel(channel_r(source), channel_r(destination), alpha),
        blend_channel(alpha, channel_a(destination), alpha));
}

inline uint premultiply_coverage(uint color, uint coverage) {
    uint alpha = mul_u8(channel_a(color), coverage);
    return pack_bgra(
        mul_u8(channel_b(color), alpha),
        mul_u8(channel_g(color), alpha),
        mul_u8(channel_r(color), alpha),
        alpha);
}

kernel void zevryon_integer_composer(
    texture2d<uint, access::read_write> output [[texture(0)]],
    texture2d_array<uint, access::read> atlas [[texture(1)]],
    constant PushConstants& pc [[buffer(0)]],
    uint2 local [[thread_position_in_grid]]) {
    uint surface_width = pc.a.x;
    uint surface_height = pc.a.y;
    uint operation = pc.a.z;

    if (operation == 0U) {
        if (local.x < surface_width && local.y < surface_height) {
            output.write(uint4(0U), local);
        }
        return;
    }

    uint dispatch_width = pc.b.z;
    uint dispatch_height = pc.b.w;
    if (local.x >= dispatch_width || local.y >= dispatch_height) {
        return;
    }

    uint2 output_pixel = uint2(pc.b.x + local.x, pc.b.y + local.y);
    if (output_pixel.x >= surface_width || output_pixel.y >= surface_height) {
        return;
    }

    uint composed = output.read(output_pixel).x;
    int destination_x = int(pc.c.x);
    int destination_y = int(pc.c.y);
    int destination_width = int(pc.c.z);
    int destination_height = int(pc.c.w);

    if (operation == 1U) {
        composed = blend_pixel(composed, pc.e.y);
    } else if (operation == 2U) {
        uint local_x = uint(int(output_pixel.x) - destination_x);
        uint local_y = uint(int(output_pixel.y) - destination_y);
        uint source_x = pc.d.y + (local_x * pc.d.w) / uint(destination_width);
        uint source_y = pc.d.z + (local_y * pc.e.x) / uint(destination_height);
        uint texel = atlas.read(uint2(source_x, source_y), pc.d.x).x;
        uint color = pc.e.y;
        uint format = pc.e.z;
        uint source = 0U;
        if (format == 0U) {
            source = premultiply_coverage(color, channel_a(texel));
        } else if (format == 1U) {
            uint coverage_b = channel_b(texel);
            uint coverage_g = channel_g(texel);
            uint coverage_r = channel_r(texel);
            uint modulation_alpha = channel_a(color);
            uint alpha = mul_u8(
                modulation_alpha,
                max(coverage_b, max(coverage_g, coverage_r)));
            source = pack_bgra(
                mul_u8(mul_u8(channel_b(color), modulation_alpha), coverage_b),
                mul_u8(mul_u8(channel_g(color), modulation_alpha), coverage_g),
                mul_u8(mul_u8(channel_r(color), modulation_alpha), coverage_r),
                alpha);
        } else {
            uint modulation_alpha = channel_a(color);
            source = pack_bgra(
                mul_u8(channel_b(texel), modulation_alpha),
                mul_u8(channel_g(texel), modulation_alpha),
                mul_u8(channel_r(texel), modulation_alpha),
                mul_u8(channel_a(texel), modulation_alpha));
        }
        composed = blend_pixel(composed, source);
    }

    output.write(uint4(composed, 0U, 0U, 0U), output_pixel);
}
