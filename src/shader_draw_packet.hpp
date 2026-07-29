#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

enum class ShaderPrimitiveKind : std::uint8_t {
    Fill = 0U,
    GlyphBatch = 1U
};

enum class ShaderLayer : std::uint8_t {
    Selection = 0U,
    Glyph = 1U,
    Caret = 2U
};

enum class ShaderAtlasFormat : std::uint8_t {
    Alpha8 = 0U,
    LcdRgb8 = 1U,
    Bgra8 = 2U
};

enum ShaderPacketFlags : std::uint32_t {
    kShaderPacketPremultipliedAlpha = 1U << 0U,
    kShaderPacketTopLeftOrigin = 1U << 1U,
    kShaderPacketNearestAtlasSampling = 1U << 2U
};

struct ShaderRectI final {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t width{0};
    std::int32_t height{0};
};
static_assert(sizeof(ShaderRectI) == 16U);

struct ShaderColorBgra8 final {
    std::uint8_t blue{0U};
    std::uint8_t green{0U};
    std::uint8_t red{0U};
    std::uint8_t alpha{0U};
};
static_assert(sizeof(ShaderColorBgra8) == 4U);

struct ShaderPacketLimits final {
    std::uint32_t maximum_commands{0U};
    std::uint32_t maximum_scissors{0U};
    std::uint32_t maximum_fill_instances{0U};
    std::uint32_t maximum_glyph_instances{0U};
    std::uint32_t maximum_atlas_uploads{0U};
    std::uint32_t maximum_atlas_pages{0U};
    std::uint32_t maximum_surface_width{0U};
    std::uint32_t maximum_surface_height{0U};
    std::uint64_t maximum_packet_bytes{0U};
    std::uint64_t maximum_upload_payload_bytes{0U};
    std::uint64_t maximum_resident_atlas_bytes{0U};
    std::uint64_t reserved{0U};
};
static_assert(sizeof(ShaderPacketLimits) == 64U);

struct ShaderSurface final {
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint32_t flags{kShaderPacketPremultipliedAlpha |
                        kShaderPacketTopLeftOrigin |
                        kShaderPacketNearestAtlasSampling};
    std::uint32_t reserved{0U};
};
static_assert(sizeof(ShaderSurface) == 16U);

struct ShaderFillSource final {
    ShaderRectI destination;
    ShaderRectI clip;
    ShaderColorBgra8 color;
    ShaderLayer layer{ShaderLayer::Selection};
    std::uint8_t reserved0[3]{0U, 0U, 0U};
    std::uint32_t stable_id{0U};
};
static_assert(sizeof(ShaderFillSource) == 44U);

struct ShaderGlyphSource final {
    ShaderRectI destination;
    ShaderRectI clip;
    std::uint32_t atlas_page_index{0U};
    std::uint32_t atlas_page_generation{0U};
    std::uint16_t atlas_x{0U};
    std::uint16_t atlas_y{0U};
    std::uint16_t atlas_width{0U};
    std::uint16_t atlas_height{0U};
    ShaderColorBgra8 color;
    ShaderAtlasFormat format{ShaderAtlasFormat::Alpha8};
    ShaderLayer layer{ShaderLayer::Glyph};
    std::uint8_t reserved0[2]{0U, 0U};
    std::uint32_t stable_id{0U};
};
static_assert(sizeof(ShaderGlyphSource) == 60U);

struct ShaderSourceCommand final {
    ShaderPrimitiveKind kind{ShaderPrimitiveKind::Fill};
    ShaderLayer layer{ShaderLayer::Selection};
    std::uint8_t reserved0[2]{0U, 0U};
    std::uint32_t first_source{0U};
    std::uint32_t source_count{0U};
    std::uint32_t stable_id{0U};
};
static_assert(sizeof(ShaderSourceCommand) == 16U);

struct ShaderAtlasUploadSource final {
    std::uint32_t page_index{0U};
    std::uint32_t page_generation{0U};
    std::uint16_t width{0U};
    std::uint16_t height{0U};
    std::uint32_t row_bytes{0U};
    ShaderAtlasFormat format{ShaderAtlasFormat::Alpha8};
    std::uint8_t reserved0[3]{0U, 0U, 0U};
    std::uint64_t payload_offset{0U};
    std::uint64_t payload_size{0U};
    std::uint64_t payload_checksum{0U};
};
static_assert(sizeof(ShaderAtlasUploadSource) == 48U);

struct ShaderPacketInput final {
    ShaderSurface surface;
    ShaderPacketLimits limits;
    std::uint64_t frame_id{0U};
    std::uint64_t atlas_generation{0U};
    std::span<const ShaderSourceCommand> commands;
    std::span<const ShaderFillSource> fills;
    std::span<const ShaderGlyphSource> glyphs;
    std::span<const ShaderAtlasUploadSource> uploads;
    std::span<const std::byte> upload_payload;
};

struct GpuShaderScissor final {
    ShaderRectI rect;
};
static_assert(sizeof(GpuShaderScissor) == 16U);

struct GpuShaderFillInstance final {
    ShaderRectI destination;
    ShaderColorBgra8 color;
    std::uint32_t scissor_index{0U};
    std::uint32_t stable_id{0U};
    std::uint32_t reserved{0U};
};
static_assert(sizeof(GpuShaderFillInstance) == 32U);

struct GpuShaderGlyphInstance final {
    ShaderRectI destination;
    std::uint32_t scissor_index{0U};
    std::uint32_t atlas_page_index{0U};
    std::uint32_t atlas_page_generation{0U};
    std::uint16_t atlas_x{0U};
    std::uint16_t atlas_y{0U};
    std::uint16_t atlas_width{0U};
    std::uint16_t atlas_height{0U};
    ShaderColorBgra8 color;
    ShaderAtlasFormat format{ShaderAtlasFormat::Alpha8};
    std::uint8_t reserved0[3]{0U, 0U, 0U};
    std::uint32_t stable_id{0U};
    std::uint32_t reserved1[4]{0U, 0U, 0U, 0U};
};
static_assert(sizeof(GpuShaderGlyphInstance) == 64U);

struct GpuShaderAtlasUpload final {
    std::uint32_t page_index{0U};
    std::uint32_t page_generation{0U};
    std::uint16_t width{0U};
    std::uint16_t height{0U};
    std::uint32_t row_bytes{0U};
    ShaderAtlasFormat format{ShaderAtlasFormat::Alpha8};
    std::uint8_t reserved0[3]{0U, 0U, 0U};
    std::uint64_t payload_offset{0U};
    std::uint64_t payload_size{0U};
    std::uint64_t canonical_page_bytes{0U};
    std::uint64_t payload_checksum{0U};
};
static_assert(sizeof(GpuShaderAtlasUpload) == 56U);

struct GpuShaderDrawCommand final {
    ShaderPrimitiveKind kind{ShaderPrimitiveKind::Fill};
    ShaderLayer layer{ShaderLayer::Selection};
    ShaderAtlasFormat atlas_format{ShaderAtlasFormat::Alpha8};
    std::uint8_t reserved0{0U};
    std::uint32_t first_instance{0U};
    std::uint32_t instance_count{0U};
    std::uint32_t atlas_page_index{0U};
    std::uint32_t scissor_index{0U};
    std::uint32_t stable_id{0U};
    std::uint32_t reserved1{0U};
    std::uint32_t reserved2{0U};
};
static_assert(sizeof(GpuShaderDrawCommand) == 32U);

struct GpuShaderPacketHeader final {
    std::uint64_t frame_id{0U};
    std::uint64_t atlas_generation{0U};
    std::uint64_t packet_checksum{0U};
    std::uint64_t packet_bytes{0U};
    std::uint64_t upload_payload_bytes{0U};
    std::uint32_t surface_width{0U};
    std::uint32_t surface_height{0U};
    std::uint32_t flags{0U};
    std::uint32_t command_count{0U};
    std::uint32_t fill_instance_count{0U};
    std::uint32_t glyph_instance_count{0U};
    std::uint32_t scissor_count{0U};
    std::uint32_t upload_count{0U};
};
static_assert(sizeof(GpuShaderPacketHeader) == 72U);

struct GpuShaderPacket final {
    explicit GpuShaderPacket(std::pmr::memory_resource* resource);

    GpuShaderPacketHeader header;
    std::pmr::vector<GpuShaderScissor> scissors;
    std::pmr::vector<GpuShaderFillInstance> fills;
    std::pmr::vector<GpuShaderGlyphInstance> glyphs;
    std::pmr::vector<GpuShaderAtlasUpload> uploads;
    std::pmr::vector<GpuShaderDrawCommand> commands;
    std::pmr::vector<std::byte> upload_payload;

    void clear() noexcept;
};

enum class ShaderPacketErrorKind : std::uint8_t {
    None = 0U,
    InvalidInput,
    InvalidOrdering,
    InvalidAtlasReference,
    ResourceBudgetExceeded,
    AllocationFailed,
    ChecksumMismatch
};

struct ShaderPacketError final {
    ShaderPacketErrorKind kind{ShaderPacketErrorKind::None};
    std::uint8_t reserved[7]{0U, 0U, 0U, 0U, 0U, 0U, 0U};
    std::string message;
};

bool compile_gpu_shader_packet(
    const ShaderPacketInput& input,
    GpuShaderPacket* output,
    ShaderPacketError* error) noexcept;

struct ShaderAtlasResidentPage final {
    std::uint32_t page_index{0U};
    std::uint32_t page_generation{0U};
    std::uint16_t width{0U};
    std::uint16_t height{0U};
    std::uint64_t last_used_frame{0U};
    std::vector<std::byte> canonical_bgra;
};

class ShaderAtlasResidency final {
public:
    ShaderAtlasResidency(
        std::uint32_t maximum_pages,
        std::uint64_t maximum_bytes);

    bool apply_packet_uploads(
        const GpuShaderPacket& packet,
        ShaderPacketError* error) noexcept;

    const ShaderAtlasResidentPage* find(
        std::uint32_t page_index,
        std::uint32_t page_generation) const noexcept;

    void mark_packet_pages_used(const GpuShaderPacket& packet) noexcept;
    void evict_before_frame(std::uint64_t minimum_frame) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::uint64_t resident_bytes() const noexcept;
    [[nodiscard]] std::uint32_t resident_pages() const noexcept;

private:
    std::uint32_t maximum_pages_{0U};
    std::uint64_t maximum_bytes_{0U};
    std::vector<ShaderAtlasResidentPage> pages_;
};

struct ShaderReadback final {
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint32_t row_bytes{0U};
    std::uint32_t reserved{0U};
    std::uint64_t checksum{0U};
    std::vector<std::byte> bgra;
};

bool execute_shader_packet_reference(
    const GpuShaderPacket& packet,
    const ShaderAtlasResidency& atlas,
    ShaderReadback* readback,
    ShaderPacketError* error) noexcept;

std::uint64_t shader_packet_checksum(const GpuShaderPacket& packet) noexcept;
std::uint64_t shader_bytes_checksum(std::span<const std::byte> bytes) noexcept;

} // namespace zevryon::text
