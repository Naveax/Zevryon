#include "shared_pixel_compositor.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace zevryon::text {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint64_t kBytesPerPixel = 4U;

void clear_error(SharedPixelCompositorError* error) noexcept {
    if (error != nullptr) {
        error->kind = SharedPixelCompositorErrorKind::None;
        error->command_index = 0U;
        error->batch_index = 0U;
        error->instance_index = 0U;
        error->upload_index = 0U;
        error->page_index = 0U;
        error->message.clear();
    }
}

bool fail(
    SharedPixelCompositorError* error,
    SharedPixelCompositorErrorKind kind,
    const char* message) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool checked_mul(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    if (left != 0U &&
        right > (std::numeric_limits<std::uint64_t>::max)() / left) {
        return false;
    }
    *output = left * right;
    return true;
}

bool checked_add(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr ||
        right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        return false;
    }
    *output = left + right;
    return true;
}

std::uint8_t channel_r(std::uint32_t rgba) noexcept {
    return static_cast<std::uint8_t>((rgba >> 24U) & 0xFFU);
}
std::uint8_t channel_g(std::uint32_t rgba) noexcept {
    return static_cast<std::uint8_t>((rgba >> 16U) & 0xFFU);
}
std::uint8_t channel_b(std::uint32_t rgba) noexcept {
    return static_cast<std::uint8_t>((rgba >> 8U) & 0xFFU);
}
std::uint8_t channel_a(std::uint32_t rgba) noexcept {
    return static_cast<std::uint8_t>(rgba & 0xFFU);
}

std::uint8_t mul255(std::uint8_t left, std::uint8_t right) noexcept {
    const std::uint32_t product =
        static_cast<std::uint32_t>(left) *
        static_cast<std::uint32_t>(right);
    return static_cast<std::uint8_t>((product + 127U) / 255U);
}

struct PremulColor final {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
    std::uint8_t a{0};
};

PremulColor premultiply(std::uint32_t rgba) noexcept {
    const std::uint8_t alpha = channel_a(rgba);
    return PremulColor{
        mul255(channel_r(rgba), alpha),
        mul255(channel_g(rgba), alpha),
        mul255(channel_b(rgba), alpha),
        alpha};
}

PremulColor load_pixel(
    const std::byte* pixel,
    GpuSurfaceFormat format) noexcept {
    if (format == GpuSurfaceFormat::Rgba8Unorm) {
        return PremulColor{
            std::to_integer<std::uint8_t>(pixel[0]),
            std::to_integer<std::uint8_t>(pixel[1]),
            std::to_integer<std::uint8_t>(pixel[2]),
            std::to_integer<std::uint8_t>(pixel[3])};
    }
    return PremulColor{
        std::to_integer<std::uint8_t>(pixel[2]),
        std::to_integer<std::uint8_t>(pixel[1]),
        std::to_integer<std::uint8_t>(pixel[0]),
        std::to_integer<std::uint8_t>(pixel[3])};
}

void store_pixel(
    std::byte* pixel,
    GpuSurfaceFormat format,
    const PremulColor& value) noexcept {
    if (format == GpuSurfaceFormat::Rgba8Unorm) {
        pixel[0] = static_cast<std::byte>(value.r);
        pixel[1] = static_cast<std::byte>(value.g);
        pixel[2] = static_cast<std::byte>(value.b);
        pixel[3] = static_cast<std::byte>(value.a);
        return;
    }
    pixel[0] = static_cast<std::byte>(value.b);
    pixel[1] = static_cast<std::byte>(value.g);
    pixel[2] = static_cast<std::byte>(value.r);
    pixel[3] = static_cast<std::byte>(value.a);
}

void blend_pixel(
    std::byte* destination,
    GpuSurfaceFormat format,
    const PremulColor& source) noexcept {
    const PremulColor target = load_pixel(destination, format);
    const std::uint8_t inverse =
        static_cast<std::uint8_t>(255U - source.a);
    const auto combine = [inverse](std::uint8_t source_channel,
                                   std::uint8_t target_channel) noexcept {
        const std::uint32_t value =
            static_cast<std::uint32_t>(source_channel) +
            static_cast<std::uint32_t>(mul255(target_channel, inverse));
        return static_cast<std::uint8_t>(std::min<std::uint32_t>(255U, value));
    };
    store_pixel(
        destination,
        format,
        PremulColor{
            combine(source.r, target.r),
            combine(source.g, target.g),
            combine(source.b, target.b),
            combine(source.a, target.a)});
}

const SharedPixelStyle* find_style(
    std::span<const SharedPixelStyle> styles,
    std::uint32_t style_id) noexcept {
    const auto iterator = std::lower_bound(
        styles.begin(), styles.end(), style_id,
        [](const SharedPixelStyle& style, std::uint32_t value) noexcept {
            return style.style_id < value;
        });
    return iterator != styles.end() && iterator->style_id == style_id
        ? &*iterator
        : nullptr;
}

bool styles_valid(std::span<const SharedPixelStyle> styles) noexcept {
    if (styles.empty()) {
        return false;
    }
    for (std::size_t index = 1U; index < styles.size(); ++index) {
        if (styles[index - 1U].style_id >= styles[index].style_id) {
            return false;
        }
    }
    return true;
}

struct ClippedRect final {
    std::uint32_t x0{0};
    std::uint32_t y0{0};
    std::uint32_t x1{0};
    std::uint32_t y1{0};
};

bool clip_rect(
    std::int64_t x,
    std::int64_t y,
    std::uint64_t width,
    std::uint64_t height,
    const TextPaintClipRect& clip,
    const GpuSurfaceDescriptor& surface,
    ClippedRect* output) noexcept {
    if (output == nullptr || width == 0U || height == 0U) {
        return false;
    }
    const auto saturating_end = [](std::int64_t start,
                                   std::uint64_t extent) noexcept {
        if (extent > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            return (std::numeric_limits<std::int64_t>::max)();
        }
        const std::int64_t signed_extent = static_cast<std::int64_t>(extent);
        if (start > (std::numeric_limits<std::int64_t>::max)() -
                        signed_extent) {
            return (std::numeric_limits<std::int64_t>::max)();
        }
        return start + signed_extent;
    };
    const std::int64_t x_end = saturating_end(x, width);
    const std::int64_t y_end = saturating_end(y, height);
    const std::int64_t clip_x_end =
        saturating_end(clip.viewport_inline_start, clip.inline_size);
    const std::int64_t clip_y_end =
        saturating_end(clip.viewport_block_start, clip.block_size);
    const std::int64_t left = std::max<std::int64_t>(
        0, std::max(x, clip.viewport_inline_start));
    const std::int64_t top = std::max<std::int64_t>(
        0, std::max(y, clip.viewport_block_start));
    const std::int64_t right = std::min<std::int64_t>(
        static_cast<std::int64_t>(surface.width),
        std::min(x_end, clip_x_end));
    const std::int64_t bottom = std::min<std::int64_t>(
        static_cast<std::int64_t>(surface.height),
        std::min(y_end, clip_y_end));
    if (right <= left || bottom <= top) {
        return false;
    }
    output->x0 = static_cast<std::uint32_t>(left);
    output->y0 = static_cast<std::uint32_t>(top);
    output->x1 = static_cast<std::uint32_t>(right);
    output->y1 = static_cast<std::uint32_t>(bottom);
    return true;
}

std::uint64_t hash_bytes(std::span<const std::byte> bytes) noexcept {
    std::uint64_t checksum = kFnvOffset;
    for (const std::byte value : bytes) {
        checksum ^= std::to_integer<std::uint8_t>(value);
        checksum *= kFnvPrime;
    }
    return checksum;
}

bool upload_record_valid(
    const GlyphAtlasUploadRecord& upload,
    std::span<const std::byte> payload,
    const SharedPixelCompositorConfig& config) noexcept {
    if (upload.atlas_generation_id == 0U ||
        upload.page_generation == 0U ||
        upload.page_index >= config.maximum_pages ||
        upload.width == 0U || upload.height == 0U ||
        upload.atlas_x > config.page_width ||
        upload.atlas_y > config.page_height ||
        upload.width > config.page_width - upload.atlas_x ||
        upload.height > config.page_height - upload.atlas_y ||
        upload.format == GlyphRasterFormat::Empty) {
        return false;
    }
    const std::uint64_t channels =
        upload.format == GlyphRasterFormat::Alpha8 ? 1U :
        upload.format == GlyphRasterFormat::LcdRgb8 ? 3U : 4U;
    std::uint64_t minimum_row = 0U;
    if (!checked_mul(upload.width, channels, &minimum_row) ||
        upload.row_bytes < minimum_row) {
        return false;
    }
    std::uint64_t minimum_size = 0U;
    if (!checked_mul(upload.row_bytes, upload.height, &minimum_size) ||
        upload.payload_size < minimum_size) {
        return false;
    }
    std::uint64_t payload_end = 0U;
    return checked_add(
               upload.payload_offset, upload.payload_size, &payload_end) &&
        payload_end <= payload.size();
}

void clear_page(
    SharedPixelCompositor::AtlasPage* page,
    std::uint64_t atlas_generation,
    std::uint64_t page_generation,
    GlyphRasterFormat format) noexcept {
    std::fill(page->texels.begin(), page->texels.end(), std::byte{0});
    page->atlas_generation_id = atlas_generation;
    page->page_generation = page_generation;
    page->format = format;
    page->initialized = 1U;
}

void apply_upload(
    const GlyphAtlasUploadRecord& upload,
    std::span<const std::byte> payload,
    const SharedPixelCompositorConfig& config,
    SharedPixelCompositor::AtlasPage* page) noexcept {
    if (page->initialized == 0U ||
        page->atlas_generation_id != upload.atlas_generation_id ||
        page->page_generation != upload.page_generation ||
        page->format != upload.format) {
        clear_page(
            page, upload.atlas_generation_id,
            upload.page_generation, upload.format);
    }
    const std::byte* source = payload.data() + upload.payload_offset;
    for (std::uint32_t row = 0U; row < upload.height; ++row) {
        for (std::uint32_t column = 0U; column < upload.width; ++column) {
            const std::size_t target_index =
                (static_cast<std::size_t>(upload.atlas_y + row) *
                     config.page_width +
                 static_cast<std::size_t>(upload.atlas_x + column)) *
                4U;
            std::byte* target = page->texels.data() + target_index;
            if (upload.format == GlyphRasterFormat::Alpha8) {
                target[0] = std::byte{0};
                target[1] = std::byte{0};
                target[2] = std::byte{0};
                target[3] = source[
                    static_cast<std::size_t>(row) * upload.row_bytes +
                    column];
            } else if (upload.format == GlyphRasterFormat::LcdRgb8) {
                const std::size_t source_index =
                    static_cast<std::size_t>(row) * upload.row_bytes +
                    static_cast<std::size_t>(column) * 3U;
                target[0] = source[source_index + 0U];
                target[1] = source[source_index + 1U];
                target[2] = source[source_index + 2U];
                target[3] = std::byte{0xFF};
            } else {
                const std::size_t source_index =
                    static_cast<std::size_t>(row) * upload.row_bytes +
                    static_cast<std::size_t>(column) * 4U;
                std::memcpy(target, source + source_index, 4U);
            }
        }
    }
}

bool validate_request(
    const SharedPixelCompositorRequest& request,
    const SharedPixelCompositorConfig& config,
    std::span<const SharedPixelCompositor::AtlasPage> pages,
    SharedPixelCompositorError* error) noexcept {
    if (request.frame == nullptr ||
        request.frame->surface.surface_id == 0U ||
        request.frame->surface.generation_id == 0U ||
        request.frame->surface.width == 0U ||
        request.frame->surface.height == 0U ||
        request.frame->commands.size() > request.limits.maximum_commands ||
        request.frame->fill_rects.size() > request.limits.maximum_fill_rects ||
        request.frame->glyph_batches.size() >
            request.limits.maximum_glyph_batches ||
        request.draw_instances.size() >
            request.limits.maximum_draw_instances ||
        request.styles.size() > config.maximum_styles ||
        !styles_valid(request.styles)) {
        return fail(error, SharedPixelCompositorErrorKind::InvalidInput,
                    "invalid shared pixel compositor request");
    }
    std::uint64_t row_bytes = 0U;
    std::uint64_t surface_bytes = 0U;
    if (!checked_mul(request.frame->surface.width, kBytesPerPixel, &row_bytes) ||
        !checked_mul(
            row_bytes, request.frame->surface.height, &surface_bytes)) {
        return fail(error, SharedPixelCompositorErrorKind::ArithmeticOverflow,
                    "shared pixel surface byte count overflowed");
    }
    if (surface_bytes > config.maximum_surface_bytes) {
        return fail(error, SharedPixelCompositorErrorKind::ResourceLimitExceeded,
                    "shared pixel surface exceeds the configured budget");
    }
    if (request.atlas_submission != nullptr) {
        if (request.atlas_submission->atlas_generation_id == 0U ||
            request.atlas_submission->atlas_generation_id !=
                request.frame->atlas_generation_id) {
            return fail(error, SharedPixelCompositorErrorKind::StaleAtlasGeneration,
                        "frame and atlas submission generations differ");
        }
        std::uint64_t uploaded_bytes = 0U;
        for (std::size_t index = 0U;
             index < request.atlas_submission->uploads.size(); ++index) {
            const GlyphAtlasUploadRecord& upload =
                request.atlas_submission->uploads[index];
            if (!upload_record_valid(
                    upload, request.raster_payload, config)) {
                if (error != nullptr) {
                    error->upload_index = index;
                    error->page_index = upload.page_index;
                }
                return fail(error,
                            SharedPixelCompositorErrorKind::InvalidUploadTopology,
                            "invalid shared pixel atlas upload");
            }
            if (!checked_add(uploaded_bytes, upload.payload_size,
                             &uploaded_bytes) ||
                uploaded_bytes > request.limits.maximum_upload_bytes) {
                return fail(error,
                            SharedPixelCompositorErrorKind::ResourceLimitExceeded,
                            "shared pixel upload bytes exceed the limit");
            }
        }
    }
    for (std::size_t command_index = 0U;
         command_index < request.frame->commands.size();
         ++command_index) {
        const GpuFrameCommandRecord& command =
            request.frame->commands[command_index];
        if (command.clip_index >= request.frame->clips.size()) {
            if (error != nullptr) {
                error->command_index = command_index;
            }
            return fail(error,
                        SharedPixelCompositorErrorKind::InvalidFrameTopology,
                        "frame command references an invalid clip");
        }
        if (command.kind == GpuFrameCommandKind::FillRect) {
            if (command.payload_index >= request.frame->fill_rects.size()) {
                if (error != nullptr) {
                    error->command_index = command_index;
                }
                return fail(error,
                            SharedPixelCompositorErrorKind::InvalidFrameTopology,
                            "fill command references an invalid rectangle");
            }
            const TextPaintFillRect& fill =
                request.frame->fill_rects[command.payload_index];
            if (find_style(request.styles, fill.style_id) == nullptr) {
                if (error != nullptr) {
                    error->command_index = command_index;
                }
                return fail(error,
                            SharedPixelCompositorErrorKind::InvalidStyleTable,
                            "fill command style is missing");
            }
        } else if (command.kind == GpuFrameCommandKind::GlyphBatch) {
            if (command.payload_index >= request.frame->glyph_batches.size()) {
                if (error != nullptr) {
                    error->command_index = command_index;
                }
                return fail(error,
                            SharedPixelCompositorErrorKind::InvalidFrameTopology,
                            "glyph command references an invalid batch");
            }
            const GpuFrameGlyphBatch& batch =
                request.frame->glyph_batches[command.payload_index];
            const std::uint64_t instance_limit =
                static_cast<std::uint64_t>(batch.first_instance) +
                batch.instance_count;
            if (instance_limit > request.draw_instances.size() ||
                find_style(request.styles, batch.style_id) == nullptr ||
                batch.page_reference_index >=
                    request.frame->page_references.size()) {
                if (error != nullptr) {
                    error->command_index = command_index;
                    error->batch_index = command.payload_index;
                }
                return fail(error,
                            SharedPixelCompositorErrorKind::InvalidDrawTopology,
                            "glyph batch topology is invalid");
            }
            const GpuFramePageReference& reference =
                request.frame->page_references[batch.page_reference_index];
            if (reference.page_index >= config.maximum_pages ||
                reference.page_index != batch.page_index ||
                reference.page_generation != batch.page_generation) {
                if (error != nullptr) {
                    error->command_index = command_index;
                    error->batch_index = command.payload_index;
                    error->page_index = reference.page_index;
                }
                return fail(error,
                            SharedPixelCompositorErrorKind::InvalidDrawTopology,
                            "glyph batch page reference is invalid");
            }
            const SharedPixelCompositor::AtlasPage& page =
                pages[reference.page_index];
            const bool page_will_be_uploaded =
                request.atlas_submission != nullptr &&
                std::any_of(
                    request.atlas_submission->uploads.begin(),
                    request.atlas_submission->uploads.end(),
                    [&reference](const GlyphAtlasUploadRecord& upload) noexcept {
                        return upload.page_index == reference.page_index &&
                            upload.page_generation ==
                                reference.page_generation &&
                            upload.atlas_generation_id != 0U;
                    });
            if (!page_will_be_uploaded &&
                (page.initialized == 0U ||
                 page.atlas_generation_id != request.frame->atlas_generation_id ||
                 page.page_generation != reference.page_generation ||
                 page.format != reference.format)) {
                if (error != nullptr) {
                    error->command_index = command_index;
                    error->batch_index = command.payload_index;
                    error->page_index = reference.page_index;
                }
                return fail(error,
                            SharedPixelCompositorErrorKind::StaleAtlasGeneration,
                            "glyph batch samples a stale atlas page");
            }
            for (std::uint32_t offset = 0U;
                 offset < batch.instance_count; ++offset) {
                const std::size_t instance_index =
                    static_cast<std::size_t>(batch.first_instance + offset);
                const GlyphAtlasDrawInstance& instance =
                    request.draw_instances[instance_index];
                if (instance.page_index != batch.page_index ||
                    instance.page_generation != batch.page_generation ||
                    instance.atlas_generation_id !=
                        request.frame->atlas_generation_id ||
                    instance.clip_index != batch.clip_index ||
                    instance.style_id != batch.style_id ||
                    instance.atlas_x > config.page_width ||
                    instance.atlas_y > config.page_height ||
                    instance.width >
                        config.page_width - instance.atlas_x ||
                    instance.height >
                        config.page_height - instance.atlas_y) {
                    if (error != nullptr) {
                        error->command_index = command_index;
                        error->batch_index = command.payload_index;
                        error->instance_index = instance_index;
                        error->page_index = instance.page_index;
                    }
                    return fail(
                        error,
                        SharedPixelCompositorErrorKind::InvalidDrawTopology,
                        "glyph draw instance is outside the certified page");
                }
            }
        } else {
            if (error != nullptr) {
                error->command_index = command_index;
            }
            return fail(error,
                        SharedPixelCompositorErrorKind::InvalidFrameTopology,
                        "unknown shared pixel frame command");
        }
    }
    return true;
}

void draw_fill(
    const TextPaintFillRect& fill,
    const TextPaintClipRect& clip,
    std::uint32_t rgba,
    SharedCompositedFrame* output,
    SharedPixelCompositorStats* stats) noexcept {
    ClippedRect bounds;
    if (!clip_rect(
            fill.viewport_inline_start,
            fill.viewport_block_start,
            fill.inline_size,
            fill.block_size,
            clip,
            output->surface,
            &bounds)) {
        return;
    }
    const PremulColor source = premultiply(rgba);
    const bool caret = (fill.flags & kTextPaintRectCaret) != 0U;
    for (std::uint32_t y = bounds.y0; y < bounds.y1; ++y) {
        for (std::uint32_t x = bounds.x0; x < bounds.x1; ++x) {
            std::byte* pixel =
                output->pixels.data() +
                static_cast<std::size_t>(y) * output->row_bytes +
                static_cast<std::size_t>(x) * 4U;
            blend_pixel(pixel, output->surface.format, source);
            stats->blended_pixels += 1U;
            if (caret) {
                stats->caret_pixels += 1U;
            } else {
                stats->selection_pixels += 1U;
            }
        }
    }
}

PremulColor glyph_source(
    GlyphRasterFormat format,
    const std::byte* texel,
    std::uint32_t rgba) noexcept {
    const std::uint8_t style_a = channel_a(rgba);
    if (format == GlyphRasterFormat::Alpha8) {
        const std::uint8_t coverage =
            std::to_integer<std::uint8_t>(texel[3]);
        const std::uint8_t alpha = mul255(style_a, coverage);
        return PremulColor{
            mul255(channel_r(rgba), alpha),
            mul255(channel_g(rgba), alpha),
            mul255(channel_b(rgba), alpha),
            alpha};
    }
    if (format == GlyphRasterFormat::LcdRgb8) {
        const std::uint8_t cover_r =
            std::to_integer<std::uint8_t>(texel[0]);
        const std::uint8_t cover_g =
            std::to_integer<std::uint8_t>(texel[1]);
        const std::uint8_t cover_b =
            std::to_integer<std::uint8_t>(texel[2]);
        const std::uint8_t alpha = mul255(
            style_a, std::max({cover_r, cover_g, cover_b}));
        return PremulColor{
            mul255(mul255(channel_r(rgba), style_a), cover_r),
            mul255(mul255(channel_g(rgba), style_a), cover_g),
            mul255(mul255(channel_b(rgba), style_a), cover_b),
            alpha};
    }
    const std::uint8_t b = std::to_integer<std::uint8_t>(texel[0]);
    const std::uint8_t g = std::to_integer<std::uint8_t>(texel[1]);
    const std::uint8_t r = std::to_integer<std::uint8_t>(texel[2]);
    const std::uint8_t a = std::to_integer<std::uint8_t>(texel[3]);
    return PremulColor{
        mul255(r, a), mul255(g, a), mul255(b, a), a};
}

void draw_glyph_batch(
    const GpuFrameGlyphBatch& batch,
    const TextPaintClipRect& clip,
    std::uint32_t rgba,
    std::span<const GlyphAtlasDrawInstance> instances,
    const SharedPixelCompositor::AtlasPage& page,
    const SharedPixelCompositorConfig& config,
    SharedCompositedFrame* output,
    SharedPixelCompositorStats* stats) noexcept {
    for (std::uint32_t instance_offset = 0U;
         instance_offset < batch.instance_count;
         ++instance_offset) {
        const GlyphAtlasDrawInstance& instance =
            instances[batch.first_instance + instance_offset];
        ClippedRect bounds;
        if (!clip_rect(
                instance.viewport_inline_start,
                instance.viewport_block_start,
                instance.width,
                instance.height,
                clip,
                output->surface,
                &bounds)) {
            stats->clipped_pixels +=
                static_cast<std::uint64_t>(instance.width) *
                instance.height;
            continue;
        }
        const std::int64_t visible_width =
            static_cast<std::int64_t>(bounds.x1 - bounds.x0);
        const std::int64_t visible_height =
            static_cast<std::int64_t>(bounds.y1 - bounds.y0);
        const std::int64_t source_x_offset =
            static_cast<std::int64_t>(bounds.x0) -
            instance.viewport_inline_start;
        const std::int64_t source_y_offset =
            static_cast<std::int64_t>(bounds.y0) -
            instance.viewport_block_start;
        for (std::int64_t y = 0; y < visible_height; ++y) {
            for (std::int64_t x = 0; x < visible_width; ++x) {
                const std::uint32_t atlas_x =
                    instance.atlas_x +
                    static_cast<std::uint32_t>(source_x_offset + x);
                const std::uint32_t atlas_y =
                    instance.atlas_y +
                    static_cast<std::uint32_t>(source_y_offset + y);
                const std::size_t texel_index =
                    (static_cast<std::size_t>(atlas_y) *
                         config.page_width +
                     atlas_x) *
                    4U;
                const PremulColor source = glyph_source(
                    page.format, page.texels.data() + texel_index, rgba);
                if (source.a == 0U) {
                    continue;
                }
                std::byte* destination =
                    output->pixels.data() +
                    static_cast<std::size_t>(
                        static_cast<std::int64_t>(bounds.y0) + y) *
                        output->row_bytes +
                    static_cast<std::size_t>(
                        static_cast<std::int64_t>(bounds.x0) + x) *
                        4U;
                blend_pixel(destination, output->surface.format, source);
                stats->blended_pixels += 1U;
                if (page.format == GlyphRasterFormat::Alpha8) {
                    stats->alpha_glyph_pixels += 1U;
                } else if (page.format == GlyphRasterFormat::LcdRgb8) {
                    stats->lcd_glyph_pixels += 1U;
                } else {
                    stats->color_glyph_pixels += 1U;
                }
            }
        }
        const std::uint64_t full =
            static_cast<std::uint64_t>(instance.width) * instance.height;
        const std::uint64_t visible =
            static_cast<std::uint64_t>(bounds.x1 - bounds.x0) *
            (bounds.y1 - bounds.y0);
        stats->clipped_pixels += full - visible;
    }
}

} // namespace

SharedCompositedFrame::SharedCompositedFrame(
    std::pmr::memory_resource* resource)
    : pixels(resource) {}

std::pmr::memory_resource* SharedCompositedFrame::resource() const noexcept {
    return pixels.get_allocator().resource();
}

void SharedCompositedFrame::release() noexcept {
    surface = {};
    row_bytes = 0U;
    checksum = 0U;
    pixels.clear();
    pixels.shrink_to_fit();
}

NativeWindowPixelBufferView SharedCompositedFrame::view() const noexcept {
    NativeWindowPixelBufferView output;
    output.bytes = pixels;
    output.width = surface.width;
    output.height = surface.height;
    output.row_bytes = row_bytes;
    output.format = surface.format;
    output.premultiplied_alpha = surface.premultiplied_alpha;
    output.checksum = checksum;
    return output;
}

SharedPixelCompositor::SharedPixelCompositor(
    SharedPixelCompositorConfig config,
    std::pmr::memory_resource* resource) noexcept
    : config_(config),
      pages_(resource),
      resource_(resource) {
    std::uint64_t page_pixels = 0U;
    std::uint64_t page_bytes = 0U;
    std::uint64_t total_bytes = 0U;
    if (resource_ == nullptr ||
        config_.page_width == 0U ||
        config_.page_height == 0U ||
        config_.maximum_pages == 0U ||
        config_.maximum_styles == 0U ||
        !checked_mul(config_.page_width, config_.page_height, &page_pixels) ||
        !checked_mul(page_pixels, 4U, &page_bytes) ||
        !checked_mul(page_bytes, config_.maximum_pages, &total_bytes) ||
        total_bytes > config_.maximum_atlas_bytes) {
        config_ = {};
        return;
    }
    try {
        pages_.reserve(config_.maximum_pages);
        for (std::uint32_t index = 0U;
             index < config_.maximum_pages; ++index) {
            pages_.emplace_back(resource_);
            pages_.back().texels.resize(
                static_cast<std::size_t>(page_bytes), std::byte{0});
        }
        atlas_bytes_ = total_bytes;
    } catch (...) {
        pages_.clear();
        config_ = {};
        atlas_bytes_ = 0U;
    }
}

bool SharedPixelCompositor::clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (AtlasPage& page : pages_) {
        std::fill(page.texels.begin(), page.texels.end(), std::byte{0});
        page.atlas_generation_id = 0U;
        page.page_generation = 0U;
        page.format = GlyphRasterFormat::Empty;
        page.initialized = 0U;
    }
    atlas_generation_id_ = 0U;
    return true;
}

SharedPixelCompositorConfig SharedPixelCompositor::config() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

std::uint64_t SharedPixelCompositor::atlas_generation() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return atlas_generation_id_;
}

std::uint64_t SharedPixelCompositor::atlas_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return atlas_bytes_;
}

const char* shared_pixel_compositor_error_kind_name(
    SharedPixelCompositorErrorKind kind) noexcept {
    switch (kind) {
        case SharedPixelCompositorErrorKind::None: return "none";
        case SharedPixelCompositorErrorKind::InvalidInput:
            return "invalid_input";
        case SharedPixelCompositorErrorKind::StaleAtlasGeneration:
            return "stale_atlas_generation";
        case SharedPixelCompositorErrorKind::InvalidStyleTable:
            return "invalid_style_table";
        case SharedPixelCompositorErrorKind::InvalidUploadTopology:
            return "invalid_upload_topology";
        case SharedPixelCompositorErrorKind::InvalidFrameTopology:
            return "invalid_frame_topology";
        case SharedPixelCompositorErrorKind::InvalidDrawTopology:
            return "invalid_draw_topology";
        case SharedPixelCompositorErrorKind::ResourceLimitExceeded:
            return "resource_limit_exceeded";
        case SharedPixelCompositorErrorKind::ArithmeticOverflow:
            return "arithmetic_overflow";
        case SharedPixelCompositorErrorKind::OutputBudgetExceeded:
            return "output_budget_exceeded";
        case SharedPixelCompositorErrorKind::AggregateOverflow:
            return "aggregate_overflow";
    }
    return "unknown";
}

bool compose_shared_pixel_frame(
    const SharedPixelCompositorRequest& request,
    SharedPixelCompositor* compositor,
    SharedCompositedFrame* output,
    SharedPixelCompositorStats* stats,
    SharedPixelCompositorError* error) noexcept {
    clear_error(error);
    if (compositor == nullptr || output == nullptr || stats == nullptr) {
        return fail(error, SharedPixelCompositorErrorKind::InvalidInput,
                    "shared pixel compositor output pointers are null");
    }
    *stats = {};
    std::lock_guard<std::mutex> lock(compositor->mutex_);
    if (compositor->config_.maximum_pages == 0U ||
        compositor->pages_.size() != compositor->config_.maximum_pages ||
        !validate_request(request, compositor->config_, compositor->pages_, error)) {
        return error != nullptr &&
                error->kind != SharedPixelCompositorErrorKind::None
            ? false
            : fail(error, SharedPixelCompositorErrorKind::InvalidInput,
                   "shared pixel compositor is not initialized");
    }

    SharedCompositedFrame staged(output->resource());
    staged.surface = request.frame->surface;
    staged.row_bytes = staged.surface.width * 4U;
    std::uint64_t surface_bytes = 0U;
    if (!checked_mul(
            staged.row_bytes, staged.surface.height, &surface_bytes) ||
        surface_bytes > compositor->config_.maximum_surface_bytes ||
        surface_bytes >
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        return fail(error, SharedPixelCompositorErrorKind::OutputBudgetExceeded,
                    "shared pixel output allocation exceeds the budget");
    }
    try {
        staged.pixels.resize(
            static_cast<std::size_t>(surface_bytes), std::byte{0});
    } catch (...) {
        return fail(error, SharedPixelCompositorErrorKind::OutputBudgetExceeded,
                    "shared pixel output allocation failed");
    }

    const PremulColor background = premultiply(request.background_rgba8);
    for (std::uint32_t y = 0U; y < staged.surface.height; ++y) {
        for (std::uint32_t x = 0U; x < staged.surface.width; ++x) {
            store_pixel(
                staged.pixels.data() +
                    static_cast<std::size_t>(y) * staged.row_bytes +
                    static_cast<std::size_t>(x) * 4U,
                staged.surface.format,
                background);
        }
    }

    if (request.frame->atlas_generation_id !=
        compositor->atlas_generation_id_) {
        for (SharedPixelCompositor::AtlasPage& page : compositor->pages_) {
            std::fill(page.texels.begin(), page.texels.end(), std::byte{0});
            page.atlas_generation_id = 0U;
            page.page_generation = 0U;
            page.format = GlyphRasterFormat::Empty;
            page.initialized = 0U;
        }
        compositor->atlas_generation_id_ =
            request.frame->atlas_generation_id;
    }
    if (request.atlas_submission != nullptr) {
        for (const GlyphAtlasUploadRecord& upload :
             request.atlas_submission->uploads) {
            apply_upload(
                upload, request.raster_payload, compositor->config_,
                &compositor->pages_[upload.page_index]);
            stats->input_uploads += 1U;
            stats->uploaded_bytes += upload.payload_size;
        }
    }

    stats->input_commands = request.frame->commands.size();
    stats->input_fill_rects = request.frame->fill_rects.size();
    stats->input_glyph_batches = request.frame->glyph_batches.size();
    stats->input_draw_instances = request.draw_instances.size();
    stats->surface_bytes = surface_bytes;
    stats->atlas_bytes = compositor->atlas_bytes_;

    for (std::size_t command_index = 0U;
         command_index < request.frame->commands.size();
         ++command_index) {
        const GpuFrameCommandRecord& command =
            request.frame->commands[command_index];
        const TextPaintClipRect& clip =
            request.frame->clips[command.clip_index];
        if (command.kind == GpuFrameCommandKind::FillRect) {
            const TextPaintFillRect& fill =
                request.frame->fill_rects[command.payload_index];
            const SharedPixelStyle* style =
                find_style(request.styles, fill.style_id);
            draw_fill(fill, clip, style->rgba8, &staged, stats);
        } else {
            const GpuFrameGlyphBatch& batch =
                request.frame->glyph_batches[command.payload_index];
            const SharedPixelStyle* style =
                find_style(request.styles, batch.style_id);
            draw_glyph_batch(
                batch, clip, style->rgba8,
                request.draw_instances,
                compositor->pages_[batch.page_index],
                compositor->config_,
                &staged,
                stats);
        }
    }

    staged.checksum = hash_bytes(staged.pixels);
    stats->checksum = staged.checksum;
    output->release();
    output->surface = staged.surface;
    output->row_bytes = staged.row_bytes;
    output->checksum = staged.checksum;
    output->pixels.swap(staged.pixels);
    return true;
}

} // namespace zevryon::text
