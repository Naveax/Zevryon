#pragma once

#include "gpu_atlas_frame_submission.hpp"
#include "native_window_pixel_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

struct SharedPixelStyle final {
    std::uint32_t style_id{0};
    std::uint32_t rgba8{0};

    bool operator==(const SharedPixelStyle&) const noexcept = default;
};

static_assert(sizeof(SharedPixelStyle) == 8U);

struct SharedPixelCompositorConfig final {
    std::uint32_t page_width{0};
    std::uint32_t page_height{0};
    std::uint32_t maximum_pages{0};
    std::uint32_t maximum_styles{0};
    std::uint64_t maximum_surface_bytes{0};
    std::uint64_t maximum_atlas_bytes{0};
};

struct SharedPixelCompositorLimits final {
    std::uint32_t maximum_commands{0};
    std::uint32_t maximum_fill_rects{0};
    std::uint32_t maximum_glyph_batches{0};
    std::uint64_t maximum_draw_instances{0};
    std::uint64_t maximum_upload_bytes{0};
};

enum class SharedPixelCompositorErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    StaleAtlasGeneration,
    InvalidStyleTable,
    InvalidUploadTopology,
    InvalidFrameTopology,
    InvalidDrawTopology,
    ResourceLimitExceeded,
    ArithmeticOverflow,
    OutputBudgetExceeded,
    AggregateOverflow
};

struct SharedPixelCompositorError final {
    SharedPixelCompositorErrorKind kind{
        SharedPixelCompositorErrorKind::None};
    std::size_t command_index{0};
    std::size_t batch_index{0};
    std::size_t instance_index{0};
    std::size_t upload_index{0};
    std::uint32_t page_index{0};
    std::string message;
};

struct SharedPixelCompositorStats final {
    std::uint64_t input_commands{0};
    std::uint64_t input_fill_rects{0};
    std::uint64_t input_glyph_batches{0};
    std::uint64_t input_draw_instances{0};
    std::uint64_t input_uploads{0};
    std::uint64_t uploaded_bytes{0};
    std::uint64_t selection_pixels{0};
    std::uint64_t caret_pixels{0};
    std::uint64_t alpha_glyph_pixels{0};
    std::uint64_t lcd_glyph_pixels{0};
    std::uint64_t color_glyph_pixels{0};
    std::uint64_t clipped_pixels{0};
    std::uint64_t blended_pixels{0};
    std::uint64_t surface_bytes{0};
    std::uint64_t atlas_bytes{0};
    std::uint64_t checksum{0};
};

class SharedCompositedFrame final {
public:
    explicit SharedCompositedFrame(
        std::pmr::memory_resource* resource =
            std::pmr::get_default_resource());

    SharedCompositedFrame(const SharedCompositedFrame&) = delete;
    SharedCompositedFrame& operator=(const SharedCompositedFrame&) = delete;
    SharedCompositedFrame(SharedCompositedFrame&&) noexcept = default;
    SharedCompositedFrame& operator=(SharedCompositedFrame&&) = delete;

    std::pmr::memory_resource* resource() const noexcept;
    void release() noexcept;
    NativeWindowPixelBufferView view() const noexcept;

    GpuSurfaceDescriptor surface;
    std::uint32_t row_bytes{0};
    std::uint64_t checksum{0};
    std::pmr::vector<std::byte> pixels;
};

struct SharedPixelCompositorRequest final {
    const GpuFrameSubmission* frame{nullptr};
    const GlyphAtlasSubmission* atlas_submission{nullptr};
    std::span<const GlyphAtlasDrawInstance> draw_instances;
    std::span<const std::byte> raster_payload;
    std::span<const SharedPixelStyle> styles;
    std::uint32_t background_rgba8{0x000000FFU};
    SharedPixelCompositorLimits limits;
};

class SharedPixelCompositor final {
public:
    SharedPixelCompositor(
        SharedPixelCompositorConfig config,
        std::pmr::memory_resource* resource =
            std::pmr::get_default_resource()) noexcept;

    SharedPixelCompositor(const SharedPixelCompositor&) = delete;
    SharedPixelCompositor& operator=(const SharedPixelCompositor&) = delete;

    bool clear() noexcept;
    SharedPixelCompositorConfig config() const noexcept;
    std::uint64_t atlas_generation() const noexcept;
    std::uint64_t atlas_bytes() const noexcept;

public:
    struct AtlasPage final {
        std::uint64_t atlas_generation_id{0};
        std::uint64_t page_generation{0};
        GlyphRasterFormat format{GlyphRasterFormat::Empty};
        std::uint8_t initialized{0};
        std::uint16_t reserved{0};
        std::pmr::vector<std::byte> texels;

        explicit AtlasPage(std::pmr::memory_resource* resource)
            : texels(resource) {}
    };

private:
    friend bool compose_shared_pixel_frame(
        const SharedPixelCompositorRequest&,
        SharedPixelCompositor*,
        SharedCompositedFrame*,
        SharedPixelCompositorStats*,
        SharedPixelCompositorError*) noexcept;

    mutable std::mutex mutex_;
    SharedPixelCompositorConfig config_;
    std::pmr::vector<AtlasPage> pages_;
    std::pmr::memory_resource* resource_{nullptr};
    std::uint64_t atlas_generation_id_{0};
    std::uint64_t atlas_bytes_{0};
};

const char* shared_pixel_compositor_error_kind_name(
    SharedPixelCompositorErrorKind kind) noexcept;

bool compose_shared_pixel_frame(
    const SharedPixelCompositorRequest& request,
    SharedPixelCompositor* compositor,
    SharedCompositedFrame* output,
    SharedPixelCompositorStats* stats,
    SharedPixelCompositorError* error) noexcept;

} // namespace zevryon::text
