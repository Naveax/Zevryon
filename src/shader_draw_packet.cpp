#include "shader_draw_packet.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace zevryon::text {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

bool fail(
    ShaderPacketError* error,
    ShaderPacketErrorKind kind,
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

void clear_error(ShaderPacketError* error) noexcept {
    if (error != nullptr) {
        error->kind = ShaderPacketErrorKind::None;
        error->message.clear();
    }
}

bool add_u64(
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

bool multiply_u64(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr ||
        (left != 0U && right > (std::numeric_limits<std::uint64_t>::max)() / left)) {
        return false;
    }
    *output = left * right;
    return true;
}

bool rect_valid(const ShaderRectI& rect) noexcept {
    if (rect.width <= 0 || rect.height <= 0 || rect.x < 0 || rect.y < 0) {
        return false;
    }
    const std::int64_t right =
        static_cast<std::int64_t>(rect.x) + rect.width;
    const std::int64_t bottom =
        static_cast<std::int64_t>(rect.y) + rect.height;
    return right <= (std::numeric_limits<std::int32_t>::max)() &&
           bottom <= (std::numeric_limits<std::int32_t>::max)();
}

bool rect_within_surface(
    const ShaderRectI& rect,
    const ShaderSurface& surface) noexcept {
    if (!rect_valid(rect)) {
        return false;
    }
    const std::int64_t right =
        static_cast<std::int64_t>(rect.x) + rect.width;
    const std::int64_t bottom =
        static_cast<std::int64_t>(rect.y) + rect.height;
    return right <= static_cast<std::int64_t>(surface.width) &&
           bottom <= static_cast<std::int64_t>(surface.height);
}

ShaderRectI intersect_rect(
    const ShaderRectI& left,
    const ShaderRectI& right) noexcept {
    const std::int32_t x0 = std::max(left.x, right.x);
    const std::int32_t y0 = std::max(left.y, right.y);
    const std::int32_t x1 = std::min(
        left.x + left.width,
        right.x + right.width);
    const std::int32_t y1 = std::min(
        left.y + left.height,
        right.y + right.height);
    if (x1 <= x0 || y1 <= y0) {
        return {};
    }
    return ShaderRectI{x0, y0, x1 - x0, y1 - y0};
}

bool rect_equal(const ShaderRectI& left, const ShaderRectI& right) noexcept {
    return left.x == right.x && left.y == right.y &&
           left.width == right.width && left.height == right.height;
}

std::uint32_t bytes_per_texel(ShaderAtlasFormat format) noexcept {
    switch (format) {
        case ShaderAtlasFormat::Alpha8:
            return 1U;
        case ShaderAtlasFormat::LcdRgb8:
            return 3U;
        case ShaderAtlasFormat::Bgra8:
            return 4U;
    }
    return 0U;
}

bool valid_format(ShaderAtlasFormat format) noexcept {
    return bytes_per_texel(format) != 0U;
}

bool find_or_append_scissor(
    const ShaderRectI& rect,
    const ShaderPacketLimits& limits,
    std::pmr::vector<GpuShaderScissor>* scissors,
    std::uint32_t* index,
    ShaderPacketError* error) {
    if (scissors == nullptr || index == nullptr) {
        return fail(error, ShaderPacketErrorKind::InvalidInput,
                    "null scissor publication target");
    }
    for (std::size_t candidate = 0U; candidate < scissors->size(); ++candidate) {
        if (rect_equal((*scissors)[candidate].rect, rect)) {
            *index = static_cast<std::uint32_t>(candidate);
            return true;
        }
    }
    if (scissors->size() >= limits.maximum_scissors) {
        return fail(error, ShaderPacketErrorKind::ResourceBudgetExceeded,
                    "shader packet scissor limit exceeded");
    }
    scissors->push_back(GpuShaderScissor{rect});
    *index = static_cast<std::uint32_t>(scissors->size() - 1U);
    return true;
}

template <typename T>
void hash_span(std::uint64_t* hash, std::span<const T> values) noexcept {
    const auto bytes = std::as_bytes(values);
    for (const std::byte value : bytes) {
        *hash ^= static_cast<std::uint8_t>(value);
        *hash *= kFnvPrime;
    }
}

void hash_bytes(
    std::uint64_t* hash,
    std::span<const std::byte> values) noexcept {
    for (const std::byte value : values) {
        *hash ^= static_cast<std::uint8_t>(value);
        *hash *= kFnvPrime;
    }
}

std::uint8_t mul_u8(std::uint8_t left, std::uint8_t right) noexcept {
    const std::uint32_t value =
        static_cast<std::uint32_t>(left) * right + 127U;
    return static_cast<std::uint8_t>(value / 255U);
}

std::uint8_t blend_channel(
    std::uint8_t source,
    std::uint8_t destination,
    std::uint8_t source_alpha) noexcept {
    const std::uint32_t inverse = 255U - source_alpha;
    const std::uint32_t value =
        static_cast<std::uint32_t>(source) * 255U +
        static_cast<std::uint32_t>(destination) * inverse + 127U;
    return static_cast<std::uint8_t>(std::min(255U, value / 255U));
}

void blend_pixel(
    std::byte* destination,
    ShaderColorBgra8 source) noexcept {
    auto* bytes = reinterpret_cast<std::uint8_t*>(destination);
    bytes[0] = blend_channel(source.blue, bytes[0], source.alpha);
    bytes[1] = blend_channel(source.green, bytes[1], source.alpha);
    bytes[2] = blend_channel(source.red, bytes[2], source.alpha);
    bytes[3] = blend_channel(source.alpha, bytes[3], source.alpha);
}

ShaderColorBgra8 premultiply_color(
    ShaderColorBgra8 color,
    std::uint8_t coverage) noexcept {
    const std::uint8_t alpha = mul_u8(color.alpha, coverage);
    return ShaderColorBgra8{
        mul_u8(color.blue, alpha),
        mul_u8(color.green, alpha),
        mul_u8(color.red, alpha),
        alpha};
}

} // namespace

GpuShaderPacket::GpuShaderPacket(std::pmr::memory_resource* resource)
    : scissors(resource),
      fills(resource),
      glyphs(resource),
      uploads(resource),
      commands(resource),
      upload_payload(resource) {}

void GpuShaderPacket::clear() noexcept {
    header = {};
    scissors.clear();
    fills.clear();
    glyphs.clear();
    uploads.clear();
    commands.clear();
    upload_payload.clear();
}

std::uint64_t shader_bytes_checksum(
    std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_bytes(&hash, bytes);
    return hash;
}

std::uint64_t shader_packet_checksum(const GpuShaderPacket& packet) noexcept {
    std::uint64_t hash = kFnvOffset;
    GpuShaderPacketHeader header = packet.header;
    header.packet_checksum = 0U;
    hash_span(&hash, std::span<const GpuShaderPacketHeader>(&header, 1U));
    hash_span(&hash, std::span<const GpuShaderScissor>(packet.scissors));
    hash_span(&hash, std::span<const GpuShaderFillInstance>(packet.fills));
    hash_span(&hash, std::span<const GpuShaderGlyphInstance>(packet.glyphs));
    hash_span(&hash, std::span<const GpuShaderAtlasUpload>(packet.uploads));
    hash_span(&hash, std::span<const GpuShaderDrawCommand>(packet.commands));
    hash_bytes(&hash, std::span<const std::byte>(packet.upload_payload));
    return hash;
}

bool compile_gpu_shader_packet(
    const ShaderPacketInput& input,
    GpuShaderPacket* output,
    ShaderPacketError* error) noexcept {
    clear_error(error);
    if (output == nullptr || input.frame_id == 0U || input.atlas_generation == 0U ||
        input.surface.width == 0U || input.surface.height == 0U ||
        input.surface.width > input.limits.maximum_surface_width ||
        input.surface.height > input.limits.maximum_surface_height ||
        input.limits.maximum_commands == 0U ||
        input.limits.maximum_scissors == 0U ||
        input.limits.maximum_packet_bytes == 0U ||
        input.limits.maximum_upload_payload_bytes == 0U ||
        (input.surface.flags & kShaderPacketPremultipliedAlpha) == 0U ||
        (input.surface.flags & kShaderPacketTopLeftOrigin) == 0U ||
        (input.surface.flags & kShaderPacketNearestAtlasSampling) == 0U) {
        return fail(error, ShaderPacketErrorKind::InvalidInput,
                    "invalid shader packet input or limits");
    }
    if (input.commands.size() > input.limits.maximum_commands ||
        input.fills.size() > input.limits.maximum_fill_instances ||
        input.glyphs.size() > input.limits.maximum_glyph_instances ||
        input.uploads.size() > input.limits.maximum_atlas_uploads ||
        input.upload_payload.size() >
            input.limits.maximum_upload_payload_bytes) {
        return fail(error, ShaderPacketErrorKind::ResourceBudgetExceeded,
                    "shader packet input exceeds configured limits");
    }

    try {
        GpuShaderPacket candidate(output->commands.get_allocator().resource());
        candidate.scissors.reserve(std::min<std::size_t>(
            input.commands.size(), input.limits.maximum_scissors));
        candidate.fills.reserve(input.fills.size());
        candidate.glyphs.reserve(input.glyphs.size());
        candidate.uploads.reserve(input.uploads.size());
        candidate.commands.reserve(input.commands.size());
        candidate.upload_payload.assign(
            input.upload_payload.begin(), input.upload_payload.end());

        ShaderLayer previous_layer = ShaderLayer::Selection;
        bool have_previous = false;
        std::uint32_t expected_fill = 0U;
        std::uint32_t expected_glyph = 0U;

        for (const ShaderSourceCommand& source_command : input.commands) {
            if (source_command.source_count == 0U || source_command.stable_id == 0U ||
                (have_previous &&
                 static_cast<std::uint8_t>(source_command.layer) <
                     static_cast<std::uint8_t>(previous_layer))) {
                return fail(error, ShaderPacketErrorKind::InvalidOrdering,
                            "shader source commands are not in stable layer order");
            }
            have_previous = true;
            previous_layer = source_command.layer;

            GpuShaderDrawCommand command{};
            command.kind = source_command.kind;
            command.layer = source_command.layer;
            command.stable_id = source_command.stable_id;

            if (source_command.kind == ShaderPrimitiveKind::Fill) {
                if ((source_command.layer != ShaderLayer::Selection &&
                     source_command.layer != ShaderLayer::Caret) ||
                    source_command.source_count != 1U ||
                    source_command.first_source != expected_fill ||
                    source_command.first_source >= input.fills.size()) {
                    return fail(error, ShaderPacketErrorKind::InvalidOrdering,
                                "fill commands must consume one sequential source");
                }
                const ShaderFillSource& source =
                    input.fills[source_command.first_source];
                if (source.layer != source_command.layer || source.stable_id == 0U ||
                    !rect_within_surface(source.destination, input.surface) ||
                    !rect_within_surface(source.clip, input.surface)) {
                    return fail(error, ShaderPacketErrorKind::InvalidInput,
                                "invalid fill source geometry or layer");
                }
                std::uint32_t scissor_index = 0U;
                if (!find_or_append_scissor(
                        source.clip, input.limits, &candidate.scissors,
                        &scissor_index, error)) {
                    return false;
                }
                command.first_instance =
                    static_cast<std::uint32_t>(candidate.fills.size());
                command.instance_count = 1U;
                command.scissor_index = scissor_index;
                candidate.fills.push_back(GpuShaderFillInstance{
                    source.destination,
                    source.color,
                    scissor_index,
                    source.stable_id,
                    0U});
                expected_fill += 1U;
            } else if (source_command.kind == ShaderPrimitiveKind::GlyphBatch) {
                if (source_command.layer != ShaderLayer::Glyph ||
                    source_command.first_source != expected_glyph ||
                    source_command.first_source > input.glyphs.size() ||
                    source_command.source_count >
                        input.glyphs.size() - source_command.first_source) {
                    return fail(error, ShaderPacketErrorKind::InvalidOrdering,
                                "glyph batches must consume sequential sources");
                }
                const ShaderGlyphSource& first =
                    input.glyphs[source_command.first_source];
                if (!valid_format(first.format) || first.layer != ShaderLayer::Glyph ||
                    first.stable_id == 0U || first.atlas_page_generation == 0U ||
                    first.atlas_width == 0U || first.atlas_height == 0U ||
                    !rect_within_surface(first.destination, input.surface) ||
                    !rect_within_surface(first.clip, input.surface)) {
                    return fail(error, ShaderPacketErrorKind::InvalidInput,
                                "invalid first glyph source in batch");
                }
                std::uint32_t scissor_index = 0U;
                if (!find_or_append_scissor(
                        first.clip, input.limits, &candidate.scissors,
                        &scissor_index, error)) {
                    return false;
                }
                command.first_instance =
                    static_cast<std::uint32_t>(candidate.glyphs.size());
                command.instance_count = source_command.source_count;
                command.atlas_page_index = first.atlas_page_index;
                command.atlas_format = first.format;
                command.scissor_index = scissor_index;

                for (std::uint32_t offset = 0U;
                     offset < source_command.source_count;
                     ++offset) {
                    const ShaderGlyphSource& source =
                        input.glyphs[source_command.first_source + offset];
                    if (source.layer != ShaderLayer::Glyph || source.stable_id == 0U ||
                        source.format != first.format ||
                        source.atlas_page_index != first.atlas_page_index ||
                        source.atlas_page_generation != first.atlas_page_generation ||
                        !rect_equal(source.clip, first.clip) ||
                        source.atlas_width == 0U || source.atlas_height == 0U ||
                        !rect_within_surface(source.destination, input.surface)) {
                        return fail(error, ShaderPacketErrorKind::InvalidInput,
                                    "glyph batch topology is not shader-homogeneous");
                    }
                    candidate.glyphs.push_back(GpuShaderGlyphInstance{
                        source.destination,
                        scissor_index,
                        source.atlas_page_index,
                        source.atlas_page_generation,
                        source.atlas_x,
                        source.atlas_y,
                        source.atlas_width,
                        source.atlas_height,
                        source.color,
                        source.format,
                        {0U, 0U, 0U},
                        source.stable_id,
                        {0U, 0U, 0U, 0U}});
                }
                expected_glyph += source_command.source_count;
            } else {
                return fail(error, ShaderPacketErrorKind::InvalidInput,
                            "unknown shader source command kind");
            }
            candidate.commands.push_back(command);
        }

        if (expected_fill != input.fills.size() ||
            expected_glyph != input.glyphs.size()) {
            return fail(error, ShaderPacketErrorKind::InvalidOrdering,
                        "shader source arrays were not consumed exactly");
        }

        std::uint64_t payload_cursor = 0U;
        for (const ShaderAtlasUploadSource& source : input.uploads) {
            if (!valid_format(source.format) || source.page_generation == 0U ||
                source.width == 0U || source.height == 0U ||
                source.page_index >= input.limits.maximum_atlas_pages ||
                source.payload_offset != payload_cursor) {
                return fail(error, ShaderPacketErrorKind::InvalidAtlasReference,
                            "invalid or non-canonical atlas upload source");
            }
            const std::uint32_t texel_bytes = bytes_per_texel(source.format);
            const std::uint64_t tight_row =
                static_cast<std::uint64_t>(source.width) * texel_bytes;
            std::uint64_t required_payload = 0U;
            if (source.row_bytes < tight_row ||
                !multiply_u64(source.row_bytes, source.height, &required_payload) ||
                required_payload != source.payload_size ||
                source.payload_offset > input.upload_payload.size() ||
                source.payload_size >
                    input.upload_payload.size() - source.payload_offset) {
                return fail(error, ShaderPacketErrorKind::InvalidAtlasReference,
                            "atlas upload payload range or stride is invalid");
            }
            const auto payload = input.upload_payload.subspan(
                static_cast<std::size_t>(source.payload_offset),
                static_cast<std::size_t>(source.payload_size));
            if (shader_bytes_checksum(payload) != source.payload_checksum) {
                return fail(error, ShaderPacketErrorKind::ChecksumMismatch,
                            "atlas upload payload checksum mismatch");
            }
            std::uint64_t canonical_bytes = 0U;
            if (!multiply_u64(source.width, source.height, &canonical_bytes) ||
                !multiply_u64(canonical_bytes, 4U, &canonical_bytes)) {
                return fail(error, ShaderPacketErrorKind::ResourceBudgetExceeded,
                            "canonical atlas page byte count overflow");
            }
            candidate.uploads.push_back(GpuShaderAtlasUpload{
                source.page_index,
                source.page_generation,
                source.width,
                source.height,
                source.row_bytes,
                source.format,
                {0U, 0U, 0U},
                source.payload_offset,
                source.payload_size,
                canonical_bytes,
                source.payload_checksum});
            payload_cursor += source.payload_size;
        }
        if (payload_cursor != input.upload_payload.size()) {
            return fail(error, ShaderPacketErrorKind::InvalidAtlasReference,
                        "atlas upload payload contains unreferenced bytes");
        }

        for (const GpuShaderGlyphInstance& glyph : candidate.glyphs) {
            const auto found = std::find_if(
                candidate.uploads.begin(), candidate.uploads.end(),
                [&](const GpuShaderAtlasUpload& upload) {
                    return upload.page_index == glyph.atlas_page_index &&
                           upload.page_generation == glyph.atlas_page_generation;
                });
            if (found != candidate.uploads.end()) {
                const std::uint32_t right =
                    static_cast<std::uint32_t>(glyph.atlas_x) + glyph.atlas_width;
                const std::uint32_t bottom =
                    static_cast<std::uint32_t>(glyph.atlas_y) + glyph.atlas_height;
                if (right > found->width || bottom > found->height ||
                    glyph.format != found->format) {
                    return fail(error, ShaderPacketErrorKind::InvalidAtlasReference,
                                "glyph references outside uploaded atlas page");
                }
            }
        }

        std::uint64_t packet_bytes = sizeof(GpuShaderPacketHeader);
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 6U> arrays{{
            {candidate.scissors.size(), sizeof(GpuShaderScissor)},
            {candidate.fills.size(), sizeof(GpuShaderFillInstance)},
            {candidate.glyphs.size(), sizeof(GpuShaderGlyphInstance)},
            {candidate.uploads.size(), sizeof(GpuShaderAtlasUpload)},
            {candidate.commands.size(), sizeof(GpuShaderDrawCommand)},
            {candidate.upload_payload.size(), sizeof(std::byte)}}};
        for (const auto& [count, item_size] : arrays) {
            std::uint64_t bytes = 0U;
            if (!multiply_u64(count, item_size, &bytes) ||
                !add_u64(packet_bytes, bytes, &packet_bytes)) {
                return fail(error, ShaderPacketErrorKind::ResourceBudgetExceeded,
                            "shader packet byte count overflow");
            }
        }
        if (packet_bytes > input.limits.maximum_packet_bytes) {
            return fail(error, ShaderPacketErrorKind::ResourceBudgetExceeded,
                        "shader packet exceeds byte budget");
        }

        candidate.header.frame_id = input.frame_id;
        candidate.header.atlas_generation = input.atlas_generation;
        candidate.header.packet_bytes = packet_bytes;
        candidate.header.upload_payload_bytes = candidate.upload_payload.size();
        candidate.header.surface_width = input.surface.width;
        candidate.header.surface_height = input.surface.height;
        candidate.header.flags = input.surface.flags;
        candidate.header.command_count =
            static_cast<std::uint32_t>(candidate.commands.size());
        candidate.header.fill_instance_count =
            static_cast<std::uint32_t>(candidate.fills.size());
        candidate.header.glyph_instance_count =
            static_cast<std::uint32_t>(candidate.glyphs.size());
        candidate.header.scissor_count =
            static_cast<std::uint32_t>(candidate.scissors.size());
        candidate.header.upload_count =
            static_cast<std::uint32_t>(candidate.uploads.size());
        candidate.header.packet_checksum = shader_packet_checksum(candidate);

        std::swap(output->header, candidate.header);
        output->scissors.swap(candidate.scissors);
        output->fills.swap(candidate.fills);
        output->glyphs.swap(candidate.glyphs);
        output->uploads.swap(candidate.uploads);
        output->commands.swap(candidate.commands);
        output->upload_payload.swap(candidate.upload_payload);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, ShaderPacketErrorKind::AllocationFailed,
                    "shader packet allocation failed");
    } catch (...) {
        return fail(error, ShaderPacketErrorKind::InvalidInput,
                    "unexpected shader packet compilation failure");
    }
}

ShaderAtlasResidency::ShaderAtlasResidency(
    std::uint32_t maximum_pages,
    std::uint64_t maximum_bytes)
    : maximum_pages_(maximum_pages), maximum_bytes_(maximum_bytes) {
    pages_.reserve(maximum_pages);
}

bool ShaderAtlasResidency::apply_packet_uploads(
    const GpuShaderPacket& packet,
    ShaderPacketError* error) noexcept {
    clear_error(error);
    if (packet.header.packet_checksum != shader_packet_checksum(packet)) {
        return fail(error, ShaderPacketErrorKind::ChecksumMismatch,
                    "shader packet checksum changed before atlas publication");
    }
    try {
        std::vector<ShaderAtlasResidentPage> candidate = pages_;
        for (const GpuShaderAtlasUpload& upload : packet.uploads) {
            if (upload.payload_offset > packet.upload_payload.size() ||
                upload.payload_size >
                    packet.upload_payload.size() - upload.payload_offset) {
                return fail(error, ShaderPacketErrorKind::InvalidAtlasReference,
                            "atlas upload range exceeds packet payload");
            }
            const auto payload = std::span<const std::byte>(packet.upload_payload)
                                     .subspan(
                                         static_cast<std::size_t>(upload.payload_offset),
                                         static_cast<std::size_t>(upload.payload_size));
            if (shader_bytes_checksum(payload) != upload.payload_checksum) {
                return fail(error, ShaderPacketErrorKind::ChecksumMismatch,
                            "atlas upload payload changed before publication");
            }

            ShaderAtlasResidentPage page{};
            page.page_index = upload.page_index;
            page.page_generation = upload.page_generation;
            page.width = upload.width;
            page.height = upload.height;
            page.last_used_frame = packet.header.frame_id;
            page.canonical_bgra.assign(
                static_cast<std::size_t>(upload.canonical_page_bytes),
                std::byte{0});

            const std::uint32_t texel_bytes = bytes_per_texel(upload.format);
            for (std::uint32_t y = 0U; y < upload.height; ++y) {
                const std::size_t source_row =
                    static_cast<std::size_t>(y) * upload.row_bytes;
                for (std::uint32_t x = 0U; x < upload.width; ++x) {
                    const std::size_t source_offset =
                        source_row + static_cast<std::size_t>(x) * texel_bytes;
                    const std::size_t destination_offset =
                        (static_cast<std::size_t>(y) * upload.width + x) * 4U;
                    auto* destination = reinterpret_cast<std::uint8_t*>(
                        page.canonical_bgra.data() + destination_offset);
                    const auto* source = reinterpret_cast<const std::uint8_t*>(
                        payload.data() + source_offset);
                    if (upload.format == ShaderAtlasFormat::Alpha8) {
                        destination[0] = 255U;
                        destination[1] = 255U;
                        destination[2] = 255U;
                        destination[3] = source[0];
                    } else if (upload.format == ShaderAtlasFormat::LcdRgb8) {
                        destination[0] = source[2];
                        destination[1] = source[1];
                        destination[2] = source[0];
                        destination[3] = std::max({source[0], source[1], source[2]});
                    } else {
                        destination[0] = source[0];
                        destination[1] = source[1];
                        destination[2] = source[2];
                        destination[3] = source[3];
                    }
                }
            }

            const auto existing = std::find_if(
                candidate.begin(), candidate.end(),
                [&](const ShaderAtlasResidentPage& resident) {
                    return resident.page_index == page.page_index;
                });
            if (existing == candidate.end()) {
                candidate.push_back(std::move(page));
            } else {
                *existing = std::move(page);
            }
        }

        std::uint64_t bytes = 0U;
        for (const ShaderAtlasResidentPage& page : candidate) {
            if (!add_u64(bytes, page.canonical_bgra.size(), &bytes)) {
                return fail(error, ShaderPacketErrorKind::ResourceBudgetExceeded,
                            "resident atlas byte count overflow");
            }
        }
        if (candidate.size() > maximum_pages_ || bytes > maximum_bytes_) {
            return fail(error, ShaderPacketErrorKind::ResourceBudgetExceeded,
                        "resident atlas exceeds configured limits");
        }
        pages_.swap(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, ShaderPacketErrorKind::AllocationFailed,
                    "resident atlas allocation failed");
    } catch (...) {
        return fail(error, ShaderPacketErrorKind::InvalidInput,
                    "unexpected resident atlas publication failure");
    }
}

const ShaderAtlasResidentPage* ShaderAtlasResidency::find(
    std::uint32_t page_index,
    std::uint32_t page_generation) const noexcept {
    const auto found = std::find_if(
        pages_.begin(), pages_.end(),
        [&](const ShaderAtlasResidentPage& page) {
            return page.page_index == page_index &&
                   page.page_generation == page_generation;
        });
    return found == pages_.end() ? nullptr : &*found;
}

void ShaderAtlasResidency::mark_packet_pages_used(
    const GpuShaderPacket& packet) noexcept {
    for (const GpuShaderGlyphInstance& glyph : packet.glyphs) {
        const auto found = std::find_if(
            pages_.begin(), pages_.end(),
            [&](const ShaderAtlasResidentPage& page) {
                return page.page_index == glyph.atlas_page_index &&
                       page.page_generation == glyph.atlas_page_generation;
            });
        if (found != pages_.end()) {
            found->last_used_frame = std::max(
                found->last_used_frame, packet.header.frame_id);
        }
    }
}

void ShaderAtlasResidency::evict_before_frame(
    std::uint64_t minimum_frame) noexcept {
    pages_.erase(
        std::remove_if(
            pages_.begin(), pages_.end(),
            [&](const ShaderAtlasResidentPage& page) {
                return page.last_used_frame < minimum_frame;
            }),
        pages_.end());
}

void ShaderAtlasResidency::clear() noexcept {
    pages_.clear();
}

std::uint64_t ShaderAtlasResidency::resident_bytes() const noexcept {
    std::uint64_t result = 0U;
    for (const ShaderAtlasResidentPage& page : pages_) {
        result += page.canonical_bgra.size();
    }
    return result;
}

std::uint32_t ShaderAtlasResidency::resident_pages() const noexcept {
    return static_cast<std::uint32_t>(pages_.size());
}

bool execute_shader_packet_reference(
    const GpuShaderPacket& packet,
    const ShaderAtlasResidency& atlas,
    ShaderReadback* readback,
    ShaderPacketError* error) noexcept {
    clear_error(error);
    if (readback == nullptr || packet.header.frame_id == 0U ||
        packet.header.surface_width == 0U || packet.header.surface_height == 0U ||
        packet.header.packet_checksum != shader_packet_checksum(packet)) {
        return fail(error, ShaderPacketErrorKind::InvalidInput,
                    "invalid shader packet reference execution request");
    }
    try {
        std::uint64_t row_bytes = 0U;
        std::uint64_t surface_bytes = 0U;
        if (!multiply_u64(packet.header.surface_width, 4U, &row_bytes) ||
            !multiply_u64(row_bytes, packet.header.surface_height, &surface_bytes) ||
            row_bytes > (std::numeric_limits<std::uint32_t>::max)() ||
            surface_bytes > (std::numeric_limits<std::size_t>::max)()) {
            return fail(error, ShaderPacketErrorKind::ResourceBudgetExceeded,
                        "shader readback surface size overflow");
        }
        ShaderReadback candidate{};
        candidate.width = packet.header.surface_width;
        candidate.height = packet.header.surface_height;
        candidate.row_bytes = static_cast<std::uint32_t>(row_bytes);
        candidate.bgra.assign(
            static_cast<std::size_t>(surface_bytes), std::byte{0});

        const ShaderRectI surface_rect{
            0, 0,
            static_cast<std::int32_t>(candidate.width),
            static_cast<std::int32_t>(candidate.height)};

        for (const GpuShaderDrawCommand& command : packet.commands) {
            if (command.scissor_index >= packet.scissors.size()) {
                return fail(error, ShaderPacketErrorKind::InvalidInput,
                            "shader command references an invalid scissor");
            }
            const ShaderRectI scissor = packet.scissors[command.scissor_index].rect;
            const ShaderRectI clipped_scissor = intersect_rect(scissor, surface_rect);
            if (clipped_scissor.width == 0 || clipped_scissor.height == 0) {
                continue;
            }

            if (command.kind == ShaderPrimitiveKind::Fill) {
                if (command.first_instance > packet.fills.size() ||
                    command.instance_count >
                        packet.fills.size() - command.first_instance) {
                    return fail(error, ShaderPacketErrorKind::InvalidInput,
                                "fill command instance range is invalid");
                }
                for (std::uint32_t index = 0U;
                     index < command.instance_count;
                     ++index) {
                    const GpuShaderFillInstance& fill =
                        packet.fills[command.first_instance + index];
                    if (fill.scissor_index != command.scissor_index) {
                        return fail(error, ShaderPacketErrorKind::InvalidInput,
                                    "fill instance scissor differs from command");
                    }
                    const ShaderRectI draw = intersect_rect(
                        fill.destination, clipped_scissor);
                    for (std::int32_t y = draw.y; y < draw.y + draw.height; ++y) {
                        for (std::int32_t x = draw.x; x < draw.x + draw.width; ++x) {
                            const std::size_t offset =
                                static_cast<std::size_t>(y) * candidate.row_bytes +
                                static_cast<std::size_t>(x) * 4U;
                            blend_pixel(candidate.bgra.data() + offset, fill.color);
                        }
                    }
                }
            } else if (command.kind == ShaderPrimitiveKind::GlyphBatch) {
                if (command.first_instance > packet.glyphs.size() ||
                    command.instance_count >
                        packet.glyphs.size() - command.first_instance) {
                    return fail(error, ShaderPacketErrorKind::InvalidInput,
                                "glyph command instance range is invalid");
                }
                for (std::uint32_t index = 0U;
                     index < command.instance_count;
                     ++index) {
                    const GpuShaderGlyphInstance& glyph =
                        packet.glyphs[command.first_instance + index];
                    if (glyph.scissor_index != command.scissor_index ||
                        glyph.atlas_page_index != command.atlas_page_index ||
                        glyph.format != command.atlas_format) {
                        return fail(error, ShaderPacketErrorKind::InvalidInput,
                                    "glyph instance is incompatible with draw command");
                    }
                    const ShaderAtlasResidentPage* page = atlas.find(
                        glyph.atlas_page_index, glyph.atlas_page_generation);
                    if (page == nullptr) {
                        return fail(error, ShaderPacketErrorKind::InvalidAtlasReference,
                                    "glyph references a non-resident atlas page");
                    }
                    const std::uint32_t atlas_right =
                        static_cast<std::uint32_t>(glyph.atlas_x) + glyph.atlas_width;
                    const std::uint32_t atlas_bottom =
                        static_cast<std::uint32_t>(glyph.atlas_y) + glyph.atlas_height;
                    if (atlas_right > page->width || atlas_bottom > page->height) {
                        return fail(error, ShaderPacketErrorKind::InvalidAtlasReference,
                                    "glyph atlas rectangle exceeds resident page");
                    }
                    const ShaderRectI draw = intersect_rect(
                        glyph.destination, clipped_scissor);
                    for (std::int32_t y = draw.y; y < draw.y + draw.height; ++y) {
                        const std::uint32_t local_y = static_cast<std::uint32_t>(
                            y - glyph.destination.y);
                        const std::uint32_t source_y = glyph.atlas_y +
                            static_cast<std::uint32_t>(
                                (static_cast<std::uint64_t>(local_y) *
                                 glyph.atlas_height) /
                                static_cast<std::uint32_t>(glyph.destination.height));
                        for (std::int32_t x = draw.x; x < draw.x + draw.width; ++x) {
                            const std::uint32_t local_x = static_cast<std::uint32_t>(
                                x - glyph.destination.x);
                            const std::uint32_t source_x = glyph.atlas_x +
                                static_cast<std::uint32_t>(
                                    (static_cast<std::uint64_t>(local_x) *
                                     glyph.atlas_width) /
                                    static_cast<std::uint32_t>(glyph.destination.width));
                            const std::size_t atlas_offset =
                                (static_cast<std::size_t>(source_y) * page->width +
                                 source_x) * 4U;
                            const auto* texel = reinterpret_cast<const std::uint8_t*>(
                                page->canonical_bgra.data() + atlas_offset);
                            ShaderColorBgra8 source{};
                            if (glyph.format == ShaderAtlasFormat::Alpha8) {
                                source = premultiply_color(glyph.color, texel[3]);
                            } else if (glyph.format == ShaderAtlasFormat::LcdRgb8) {
                                const std::uint8_t alpha = mul_u8(
                                    glyph.color.alpha,
                                    std::max({texel[0], texel[1], texel[2]}));
                                source.blue = mul_u8(
                                    mul_u8(glyph.color.blue, glyph.color.alpha),
                                    texel[0]);
                                source.green = mul_u8(
                                    mul_u8(glyph.color.green, glyph.color.alpha),
                                    texel[1]);
                                source.red = mul_u8(
                                    mul_u8(glyph.color.red, glyph.color.alpha),
                                    texel[2]);
                                source.alpha = alpha;
                            } else {
                                const std::uint8_t modulation_alpha = glyph.color.alpha;
                                source.blue = mul_u8(texel[0], modulation_alpha);
                                source.green = mul_u8(texel[1], modulation_alpha);
                                source.red = mul_u8(texel[2], modulation_alpha);
                                source.alpha = mul_u8(texel[3], modulation_alpha);
                            }
                            const std::size_t destination_offset =
                                static_cast<std::size_t>(y) * candidate.row_bytes +
                                static_cast<std::size_t>(x) * 4U;
                            blend_pixel(candidate.bgra.data() + destination_offset, source);
                        }
                    }
                }
            } else {
                return fail(error, ShaderPacketErrorKind::InvalidInput,
                            "unknown shader packet command kind");
            }
        }

        candidate.checksum = shader_bytes_checksum(candidate.bgra);
        *readback = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, ShaderPacketErrorKind::AllocationFailed,
                    "shader reference readback allocation failed");
    } catch (...) {
        return fail(error, ShaderPacketErrorKind::InvalidInput,
                    "unexpected shader reference execution failure");
    }
}

} // namespace zevryon::text
