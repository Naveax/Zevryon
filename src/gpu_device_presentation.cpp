#include "gpu_device_presentation.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <tuple>

namespace zevryon::text {
namespace {

template <typename T>
void release_vector(std::pmr::vector<T>* values) noexcept {
    std::pmr::vector<T> empty(values->get_allocator().resource());
    values->swap(empty);
}

void clear_api_error(GpuDeviceApiError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool api_fail(
    GpuDeviceApiErrorKind kind,
    const char* message,
    GpuDeviceApiError* error) noexcept {
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

void clear_upload_error(GlyphAtlasUploadBackendError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool upload_fail(
    GlyphAtlasUploadBackendErrorKind kind,
    const char* message,
    GlyphAtlasUploadBackendError* error) noexcept {
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

void clear_frame_error(GpuFrameBackendError* error) noexcept {
    if (error != nullptr) {
        *error = {};
    }
}

bool frame_fail(
    GpuFrameBackendErrorKind kind,
    const char* message,
    GpuFrameBackendError* error) noexcept {
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


std::uint64_t fnv1a(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const std::byte value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

void mix(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (std::uint32_t byte = 0U; byte < 8U; ++byte) {
        *hash ^= (value >> (byte * 8U)) & 0xffU;
        *hash *= 1'099'511'628'211ULL;
    }
}

bool texture_matches(
    const GpuDeviceTextureRecord& record,
    std::uint64_t atlas_generation_id,
    std::uint64_t page_generation,
    std::uint32_t page_index,
    GlyphRasterFormat format,
    std::uint64_t device_generation) noexcept {
    return record.handle.device_generation == device_generation &&
        record.atlas_generation_id == atlas_generation_id &&
        record.page_generation == page_generation &&
        record.handle.page_index == page_index &&
        record.handle.format == format &&
        (record.handle.flags & kGpuDeviceTextureHandleValid) != 0U;
}

GpuDeviceTextureRecord* find_texture(
    std::pmr::vector<GpuDeviceTextureRecord>* textures,
    std::uint64_t atlas_generation_id,
    std::uint64_t page_generation,
    std::uint32_t page_index,
    GlyphRasterFormat format,
    std::uint64_t device_generation) noexcept {
    const auto it = std::find_if(
        textures->begin(),
        textures->end(),
        [&](const GpuDeviceTextureRecord& record) {
            return texture_matches(
                record,
                atlas_generation_id,
                page_generation,
                page_index,
                format,
                device_generation);
        });
    return it == textures->end() ? nullptr : &*it;
}

const GpuDeviceTextureRecord* find_texture(
    const std::pmr::vector<GpuDeviceTextureRecord>& textures,
    const GpuFramePageReference& page,
    std::uint64_t atlas_generation_id,
    std::uint64_t device_generation) noexcept {
    const auto it = std::find_if(
        textures.begin(),
        textures.end(),
        [&](const GpuDeviceTextureRecord& record) {
            return texture_matches(
                record,
                atlas_generation_id,
                page.page_generation,
                page.page_index,
                page.format,
                device_generation);
        });
    return it == textures.end() ? nullptr : &*it;
}

bool same_surface(
    const GpuSurfaceDescriptor& left,
    const GpuSurfaceDescriptor& right) noexcept {
    return left == right;
}

std::size_t count_in_flight(
    const std::pmr::vector<GpuDeviceInFlightFrameRecord>& frames) noexcept {
    return static_cast<std::size_t>(std::count_if(
        frames.begin(),
        frames.end(),
        [](const GpuDeviceInFlightFrameRecord& frame) {
            return frame.occupied != 0U;
        }));
}

} // namespace

bool ReferenceGpuDeviceApi::create_texture(
    std::uint32_t page_index,
    GlyphRasterFormat format,
    std::uint32_t width,
    std::uint32_t height,
    std::uint64_t device_generation,
    GpuDeviceTextureHandle* output,
    GpuDeviceApiError* error) noexcept {
    clear_api_error(error);
    if (output == nullptr || width == 0U || height == 0U ||
        device_generation == 0U || format == GlyphRasterFormat::Empty ||
        next_texture_id_ == 0U || next_texture_generation_ == 0U) {
        return api_fail(
            GpuDeviceApiErrorKind::InvalidInput,
            "invalid texture allocation request",
            error);
    }
    *output = {
        device_generation,
        next_texture_generation_,
        next_texture_id_,
        page_index,
        format,
        kGpuDeviceTextureHandleValid,
        0U};
    if (++next_texture_id_ == 0U || ++next_texture_generation_ == 0U) {
        return api_fail(
            GpuDeviceApiErrorKind::ResourceAllocationFailed,
            "reference texture identity overflowed",
            error);
    }
    return true;
}

void ReferenceGpuDeviceApi::release_texture(
    const GpuDeviceTextureHandle& texture) noexcept {
    (void)texture;
}

bool ReferenceGpuDeviceApi::upload_texture(
    const GpuDeviceTextureHandle& texture,
    const GlyphAtlasBackendUploadBatch& batch,
    std::span<const GlyphAtlasUploadRecord> uploads,
    std::span<const std::byte> payload,
    std::uint64_t ticket_id,
    std::uint64_t* fence_value,
    std::uint64_t* payload_checksum,
    GpuDeviceApiError* error) noexcept {
    clear_api_error(error);
    if (fence_value == nullptr || payload_checksum == nullptr || ticket_id == 0U ||
        (texture.flags & kGpuDeviceTextureHandleValid) == 0U ||
        texture.page_index != batch.page_index ||
        texture.format != batch.format || batch.upload_count == 0U ||
        batch.first_upload > uploads.size() ||
        batch.upload_count > uploads.size() - batch.first_upload) {
        return api_fail(
            GpuDeviceApiErrorKind::InvalidInput,
            "invalid texture upload request",
            error);
    }
    std::uint64_t checksum_value = 1'469'598'103'934'665'603ULL;
    for (std::size_t index = batch.first_upload;
         index < static_cast<std::size_t>(batch.first_upload) + batch.upload_count;
         ++index) {
        const GlyphAtlasUploadRecord& upload = uploads[index];
        if (upload.atlas_generation_id != batch.atlas_generation_id ||
            upload.page_generation != batch.page_generation ||
            upload.page_index != batch.page_index ||
            upload.format != batch.format ||
            upload.payload_offset > payload.size() ||
            upload.payload_size > payload.size() - upload.payload_offset) {
            return api_fail(
                GpuDeviceApiErrorKind::UploadFailed,
                "upload record is outside the requested atlas page",
                error);
        }
        const std::span<const std::byte> bytes = payload.subspan(
            static_cast<std::size_t>(upload.payload_offset),
            static_cast<std::size_t>(upload.payload_size));
        mix(&checksum_value, fnv1a(bytes));
        mix(&checksum_value, upload.atlas_x);
        mix(&checksum_value, upload.atlas_y);
        mix(&checksum_value, upload.width);
        mix(&checksum_value, upload.height);
    }
    if (next_fence_value_ == 0U) {
        return api_fail(
            GpuDeviceApiErrorKind::FenceOverflow,
            "reference upload fence overflowed",
            error);
    }
    *fence_value = next_fence_value_++;
    *payload_checksum = checksum_value;
    return true;
}

bool ReferenceGpuDeviceApi::configure_surface(
    const GpuSurfaceDescriptor& surface,
    std::uint32_t image_count,
    std::span<GpuSurfaceImageHandle> images,
    GpuDeviceApiError* error) noexcept {
    clear_api_error(error);
    if (surface.surface_id == 0U || surface.generation_id == 0U ||
        surface.width == 0U || surface.height == 0U || image_count == 0U ||
        images.size() != image_count || next_image_generation_ == 0U) {
        return api_fail(
            GpuDeviceApiErrorKind::InvalidInput,
            "invalid surface configuration",
            error);
    }
    for (std::uint32_t index = 0U; index < image_count; ++index) {
        images[index] = {
            surface.surface_id,
            surface.generation_id,
            next_image_generation_++,
            index,
            kGpuSurfaceImageHandleValid};
        if (next_image_generation_ == 0U) {
            return api_fail(
                GpuDeviceApiErrorKind::SurfaceConfigurationFailed,
                "surface image generation overflowed",
                error);
        }
    }
    return true;
}

bool ReferenceGpuDeviceApi::submit_and_present(
    const GpuSurfaceImageHandle& image,
    const GpuFrameSubmission& frame,
    std::span<const GlyphAtlasDrawInstance> draw_instances,
    std::uint64_t ticket_id,
    std::uint64_t wait_fence_value,
    std::uint64_t* signal_fence_value,
    std::uint64_t* command_checksum,
    GpuDeviceApiError* error) noexcept {
    clear_api_error(error);
    if (signal_fence_value == nullptr || command_checksum == nullptr ||
        ticket_id == 0U || (image.flags & kGpuSurfaceImageHandleValid) == 0U ||
        image.surface_id != frame.surface.surface_id ||
        image.surface_generation != frame.surface.generation_id ||
        frame.commands.empty() || frame.clips.empty()) {
        return api_fail(
            GpuDeviceApiErrorKind::InvalidInput,
            "invalid frame presentation request",
            error);
    }

    enum class Partition : std::uint8_t { Selection, Glyph, Caret };
    Partition partition = Partition::Selection;
    std::uint64_t checksum_value = 1'469'598'103'934'665'603ULL;
    mix(&checksum_value, frame.frame_id);
    mix(&checksum_value, ticket_id);
    mix(&checksum_value, wait_fence_value);
    mix(&checksum_value, image.image_index);

    for (const GpuFrameCommandRecord& command : frame.commands) {
        if (command.clip_index >= frame.clips.size()) {
            return api_fail(
                GpuDeviceApiErrorKind::PresentFailed,
                "frame command references an invalid clip",
                error);
        }
        if (command.kind == GpuFrameCommandKind::GlyphBatch) {
            if (partition == Partition::Caret ||
                command.payload_index >= frame.glyph_batches.size()) {
                return api_fail(
                    GpuDeviceApiErrorKind::PresentFailed,
                    "invalid glyph command partition",
                    error);
            }
            partition = Partition::Glyph;
            const GpuFrameGlyphBatch& batch =
                frame.glyph_batches[command.payload_index];
            if (batch.first_instance > draw_instances.size() ||
                batch.instance_count > draw_instances.size() - batch.first_instance ||
                batch.page_reference_index >= frame.page_references.size()) {
                return api_fail(
                    GpuDeviceApiErrorKind::PresentFailed,
                    "glyph batch references invalid instances or page",
                    error);
            }
            mix(&checksum_value, batch.page_generation);
            mix(&checksum_value, batch.page_index);
            mix(&checksum_value, batch.first_instance);
            mix(&checksum_value, batch.instance_count);
            mix(&checksum_value, batch.style_id);
        } else {
            if (command.payload_index >= frame.fill_rects.size()) {
                return api_fail(
                    GpuDeviceApiErrorKind::PresentFailed,
                    "fill command references an invalid rectangle",
                    error);
            }
            const TextPaintFillRect& fill = frame.fill_rects[command.payload_index];
            const bool caret = (fill.flags & kTextPaintRectCaret) != 0U;
            const bool selection = (fill.flags & kTextPaintRectSelection) != 0U;
            if (caret) {
                partition = Partition::Caret;
            } else if (!selection || partition != Partition::Selection) {
                return api_fail(
                    GpuDeviceApiErrorKind::PresentFailed,
                    "fill command violates selection-glyph-caret ordering",
                    error);
            }
            mix(&checksum_value, static_cast<std::uint64_t>(fill.viewport_inline_start));
            mix(&checksum_value, static_cast<std::uint64_t>(fill.viewport_block_start));
            mix(&checksum_value, fill.inline_size);
            mix(&checksum_value, fill.block_size);
            mix(&checksum_value, fill.style_id);
        }
    }

    if (next_fence_value_ <= wait_fence_value) {
        if (wait_fence_value == std::numeric_limits<std::uint64_t>::max()) {
            return api_fail(
                GpuDeviceApiErrorKind::FenceOverflow,
                "reference present fence overflowed",
                error);
        }
        next_fence_value_ = wait_fence_value + 1U;
    }
    if (next_fence_value_ == 0U) {
        return api_fail(
            GpuDeviceApiErrorKind::FenceOverflow,
            "reference present fence overflowed",
            error);
    }
    *signal_fence_value = next_fence_value_++;
    *command_checksum = checksum_value;
    return true;
}

GpuDevicePresentationBackend::GpuDevicePresentationBackend(
    GpuDeviceApi* api,
    GpuDevicePresentationConfig config,
    std::size_t metadata_hard_limit) noexcept
    : api_(api),
      metadata_resource_(
          ledger_,
          core::ResourceClass::CompositorSurface,
          std::pmr::get_default_resource()),
      textures_(&metadata_resource_),
      images_(&metadata_resource_),
      frames_(&metadata_resource_),
      pins_(&metadata_resource_),
      config_(config) {
    ledger_.set_hard_limit(
        core::ResourceClass::CompositorSurface,
        metadata_hard_limit);
    try {
        textures_.reserve(config.maximum_textures);
        images_.reserve(config.surface_image_count);
        frames_.reserve(config.maximum_frames_in_flight);
        pins_.reserve(config.maximum_texture_pins);
    } catch (...) {
        textures_.clear();
        images_.clear();
        frames_.clear();
        pins_.clear();
        config_ = {};
    }
}

bool GpuDevicePresentationBackend::submit(
    const GlyphAtlasBackendUploadBatch& batch,
    std::span<const GlyphAtlasUploadRecord> uploads,
    std::span<const std::byte> payload,
    std::uint64_t ticket_id,
    std::uint64_t* fence_value,
    GlyphAtlasUploadBackendError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_upload_error(error);
    if (api_ == nullptr || fence_value == nullptr || ticket_id == 0U ||
        config_.device_generation == 0U || config_.maximum_textures == 0U ||
        config_.atlas_page_width == 0U || config_.atlas_page_height == 0U ||
        batch.atlas_generation_id == 0U || batch.page_generation == 0U ||
        batch.upload_count == 0U || batch.format == GlyphRasterFormat::Empty) {
        return upload_fail(
            GlyphAtlasUploadBackendErrorKind::InvalidInput,
            "invalid bounded device upload request",
            error);
    }

    GpuDeviceTextureRecord* exact = find_texture(
        &textures_,
        batch.atlas_generation_id,
        batch.page_generation,
        batch.page_index,
        batch.format,
        config_.device_generation);

    if (exact != nullptr && exact->pin_count != 0U) {
        return upload_fail(
            GlyphAtlasUploadBackendErrorKind::SubmissionFailed,
            "cannot upload into a texture pinned by an in-flight frame",
            error);
    }

    bool new_texture = false;
    std::size_t replacement_index = textures_.size();
    GpuDeviceTextureHandle new_handle;
    if (exact == nullptr) {
        if (textures_.size() < config_.maximum_textures) {
            replacement_index = textures_.size();
        } else {
            std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
            for (std::size_t index = 0U; index < textures_.size(); ++index) {
                const GpuDeviceTextureRecord& candidate = textures_[index];
                if (candidate.pin_count == 0U &&
                    candidate.last_use_fence_value <= oldest) {
                    oldest = candidate.last_use_fence_value;
                    replacement_index = index;
                }
            }
            if (replacement_index == textures_.size()) {
                return upload_fail(
                    GlyphAtlasUploadBackendErrorKind::SubmissionFailed,
                    "all device textures are pinned",
                    error);
            }
        }
        GpuDeviceApiError api_error;
        if (!api_->create_texture(
                batch.page_index,
                batch.format,
                config_.atlas_page_width,
                config_.atlas_page_height,
                config_.device_generation,
                &new_handle,
                &api_error)) {
            return upload_fail(
                GlyphAtlasUploadBackendErrorKind::SubmissionFailed,
                api_error.message.c_str(),
                error);
        }
        new_texture = true;
    }

    GpuDeviceTextureHandle upload_handle =
        exact != nullptr ? exact->handle : new_handle;
    std::uint64_t new_fence = 0U;
    std::uint64_t checksum_value = 0U;
    GpuDeviceApiError api_error;
    if (!api_->upload_texture(
            upload_handle,
            batch,
            uploads,
            payload,
            ticket_id,
            &new_fence,
            &checksum_value,
            &api_error)) {
        if (new_texture) {
            api_->release_texture(new_handle);
        }
        return upload_fail(
            GlyphAtlasUploadBackendErrorKind::SubmissionFailed,
            api_error.message.c_str(),
            error);
    }
    if (new_fence == 0U ||
        new_fence <= completed_fence_value_ ||
        new_fence <= last_submitted_fence_value_) {
        if (new_texture) {
            api_->release_texture(new_handle);
        }
        return upload_fail(
            GlyphAtlasUploadBackendErrorKind::FenceOverflow,
            "device upload returned a non-monotone fence",
            error);
    }

    if (exact != nullptr) {
        exact->ready_fence_value = new_fence;
        exact->payload_checksum = checksum_value;
        exact->state = GpuDeviceTextureState::PendingUpload;
        ++texture_reuses_;
    } else {
        GpuDeviceTextureRecord record;
        record.handle = new_handle;
        record.atlas_generation_id = batch.atlas_generation_id;
        record.page_generation = batch.page_generation;
        record.ready_fence_value = new_fence;
        record.payload_checksum = checksum_value;
        record.state = GpuDeviceTextureState::PendingUpload;
        if (replacement_index == textures_.size()) {
            textures_.push_back(record);
        } else {
            const GpuDeviceTextureHandle old_handle =
                textures_[replacement_index].handle;
            textures_[replacement_index] = record;
            api_->release_texture(old_handle);
            ++texture_evictions_;
        }
        ++texture_allocations_;
    }
    last_submitted_fence_value_ = new_fence;
    ++upload_submissions_;
    *fence_value = new_fence;
    return true;
}

GpuFrameBackendKind GpuDevicePresentationBackend::kind() const noexcept {
    return GpuFrameBackendKind::Platform;
}

bool GpuDevicePresentationBackend::submit(
    const GpuFrameSubmission& frame,
    std::span<const GlyphAtlasDrawInstance> draw_instances,
    std::uint64_t ticket_id,
    std::uint64_t wait_fence_value,
    std::uint64_t* signal_fence_value,
    GpuFrameBackendError* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_frame_error(error);
    if (api_ == nullptr || signal_fence_value == nullptr || ticket_id == 0U ||
        config_.device_generation == 0U || config_.surface_image_count == 0U ||
        config_.maximum_frames_in_flight == 0U ||
        frame.surface.surface_id == 0U || frame.surface.generation_id == 0U ||
        frame.frame_id == 0U || frame.commands.empty()) {
        return frame_fail(
            GpuFrameBackendErrorKind::InvalidInput,
            "invalid bounded device frame request",
            error);
    }

    if (!same_surface(surface_, frame.surface)) {
        if (!frames_.empty()) {
            ++stale_rejections_;
            return frame_fail(
                GpuFrameBackendErrorKind::SubmissionFailed,
                "surface generation changed while frames are in flight",
                error);
        }
        std::pmr::vector<GpuSurfaceImageHandle> handles(&metadata_resource_);
        std::pmr::vector<GpuSurfaceImageRecord> staged_images(&metadata_resource_);
        try {
            handles.resize(config_.surface_image_count);
            staged_images.resize(config_.surface_image_count);
        } catch (...) {
            return frame_fail(
                GpuFrameBackendErrorKind::SubmissionFailed,
                "surface image metadata budget exceeded",
                error);
        }
        GpuDeviceApiError api_error;
        if (!api_->configure_surface(
                frame.surface,
                config_.surface_image_count,
                handles,
                &api_error)) {
            return frame_fail(
                GpuFrameBackendErrorKind::SubmissionFailed,
                api_error.message.c_str(),
                error);
        }
        for (std::size_t index = 0U; index < handles.size(); ++index) {
            staged_images[index].handle = handles[index];
        }
        images_.swap(staged_images);
        surface_ = frame.surface;
        ++surface_reconfigurations_;
    }

    if (frames_.size() >= config_.maximum_frames_in_flight) {
        return frame_fail(
            GpuFrameBackendErrorKind::SubmissionFailed,
            "device frame ring is full",
            error);
    }
    const auto image_it = std::find_if(
        images_.begin(),
        images_.end(),
        [](const GpuSurfaceImageRecord& image) {
            return image.in_flight == 0U;
        });
    if (image_it == images_.end()) {
        return frame_fail(
            GpuFrameBackendErrorKind::SubmissionFailed,
            "no surface image is available",
            error);
    }

    std::pmr::vector<std::uint32_t> texture_indices(&metadata_resource_);
    try {
        texture_indices.reserve(frame.page_references.size());
    } catch (...) {
        return frame_fail(
            GpuFrameBackendErrorKind::SubmissionFailed,
            "texture pin working set exceeds its budget",
            error);
    }
    for (const GpuFramePageReference& page : frame.page_references) {
        const GpuDeviceTextureRecord* texture = find_texture(
            textures_,
            page,
            frame.atlas_generation_id,
            config_.device_generation);
        if (texture == nullptr ||
            (texture->state != GpuDeviceTextureState::Resident &&
             texture->ready_fence_value > wait_fence_value)) {
            ++stale_rejections_;
            return frame_fail(
                GpuFrameBackendErrorKind::SubmissionFailed,
                "frame references a missing or not-ready device texture",
                error);
        }
        const std::uint32_t index = static_cast<std::uint32_t>(
            texture - textures_.data());
        if (std::find(texture_indices.begin(), texture_indices.end(), index) ==
            texture_indices.end()) {
            texture_indices.push_back(index);
        }
    }
    if (pins_.size() + texture_indices.size() > config_.maximum_texture_pins) {
        return frame_fail(
            GpuFrameBackendErrorKind::SubmissionFailed,
            "device texture pin capacity exceeded",
            error);
    }

    std::uint64_t new_fence = 0U;
    std::uint64_t command_checksum = 0U;
    GpuDeviceApiError api_error;
    if (!api_->submit_and_present(
            image_it->handle,
            frame,
            draw_instances,
            ticket_id,
            wait_fence_value,
            &new_fence,
            &command_checksum,
            &api_error)) {
        return frame_fail(
            GpuFrameBackendErrorKind::SubmissionFailed,
            api_error.message.c_str(),
            error);
    }
    if (new_fence == 0U ||
        new_fence <= wait_fence_value ||
        new_fence <= completed_fence_value_ ||
        new_fence <= last_submitted_fence_value_) {
        return frame_fail(
            GpuFrameBackendErrorKind::FenceOverflow,
            "device present returned a non-monotone fence",
            error);
    }

    GpuDeviceInFlightFrameRecord in_flight;
    in_flight.receipt.image = image_it->handle;
    in_flight.receipt.frame_id = frame.frame_id;
    in_flight.receipt.ticket_id = ticket_id;
    in_flight.receipt.wait_fence_value = wait_fence_value;
    in_flight.receipt.signal_fence_value = new_fence;
    in_flight.receipt.command_checksum = command_checksum;
    in_flight.receipt.command_count = static_cast<std::uint32_t>(
        frame.commands.size());
    in_flight.first_pin = static_cast<std::uint32_t>(pins_.size());
    in_flight.pin_count = static_cast<std::uint32_t>(texture_indices.size());
    in_flight.occupied = 1U;

    for (const std::uint32_t index : texture_indices) {
        GpuDeviceTextureRecord& texture = textures_[index];
        ++texture.pin_count;
        texture.last_use_fence_value = new_fence;
        pins_.push_back({
            frame.frame_id,
            texture.handle.texture_generation,
            texture.handle.resource_id,
            index,
            0U});
    }
    image_it->in_flight = 1U;
    image_it->last_submit_fence_value = new_fence;
    image_it->frame_id = frame.frame_id;
    frames_.push_back(in_flight);
    latest_receipt_ = in_flight.receipt;
    has_latest_receipt_ = true;
    last_submitted_fence_value_ = new_fence;
    ++present_submissions_;
    *signal_fence_value = new_fence;
    return true;
}

bool GpuDevicePresentationBackend::retire_completed(
    std::uint64_t completed_fence_value,
    std::string* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_fence_value < completed_fence_value_ ||
        completed_fence_value > last_submitted_fence_value_) {
        if (error != nullptr) {
            *error = completed_fence_value < completed_fence_value_
                ? "device completed fence regressed"
                : "device completed fence exceeds the submitted timeline";
        }
        return false;
    }

    std::size_t retired_frame_count = 0U;
    std::size_t retired_pin_count = 0U;
    while (retired_frame_count < frames_.size() &&
           frames_[retired_frame_count].receipt.signal_fence_value <=
               completed_fence_value) {
        const GpuDeviceInFlightFrameRecord& frame =
            frames_[retired_frame_count];
        if (frame.first_pin != retired_pin_count ||
            frame.pin_count > pins_.size() - retired_pin_count) {
            if (error != nullptr) {
                *error = "device frame pin topology is inconsistent";
            }
            return false;
        }
        for (std::size_t offset = 0U; offset < frame.pin_count; ++offset) {
            const GpuDeviceTexturePin& pin = pins_[retired_pin_count + offset];
            if (pin.texture_index >= textures_.size()) {
                if (error != nullptr) {
                    *error = "device texture pin references an invalid texture";
                }
                return false;
            }
            GpuDeviceTextureRecord& texture = textures_[pin.texture_index];
            if (texture.handle.resource_id != pin.resource_id ||
                texture.handle.texture_generation != pin.texture_generation ||
                texture.pin_count == 0U) {
                if (error != nullptr) {
                    *error = "device texture pin identity is stale";
                }
                return false;
            }
            --texture.pin_count;
        }
        const auto image = std::find_if(
            images_.begin(),
            images_.end(),
            [&](const GpuSurfaceImageRecord& candidate) {
                return candidate.handle == frame.receipt.image;
            });
        if (image == images_.end() || image->in_flight == 0U) {
            if (error != nullptr) {
                *error = "present receipt references a stale surface image";
            }
            return false;
        }
        image->in_flight = 0U;
        image->frame_id = 0U;
        retired_pin_count += frame.pin_count;
        ++retired_frame_count;
    }

    if (retired_frame_count != 0U) {
        frames_.erase(
            frames_.begin(),
            frames_.begin() + static_cast<std::ptrdiff_t>(retired_frame_count));
        pins_.erase(
            pins_.begin(),
            pins_.begin() + static_cast<std::ptrdiff_t>(retired_pin_count));
        for (GpuDeviceInFlightFrameRecord& frame : frames_) {
            frame.first_pin -= static_cast<std::uint32_t>(retired_pin_count);
        }
        retired_frames_ += retired_frame_count;
    }
    for (GpuDeviceTextureRecord& texture : textures_) {
        if (texture.state == GpuDeviceTextureState::PendingUpload &&
            texture.ready_fence_value != 0U &&
            texture.ready_fence_value <= completed_fence_value) {
            texture.state = GpuDeviceTextureState::Resident;
        }
    }
    completed_fence_value_ = completed_fence_value;
    if (has_latest_receipt_ &&
        latest_receipt_.signal_fence_value <= completed_fence_value) {
        latest_receipt_.status = GpuPresentReceiptStatus::Retired;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool GpuDevicePresentationBackend::clear(std::string* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frames_.empty() || !pins_.empty()) {
        if (error != nullptr) {
            *error = "cannot clear device state while frames are in flight";
        }
        return false;
    }
    if (api_ != nullptr) {
        for (const GpuDeviceTextureRecord& texture : textures_) {
            api_->release_texture(texture.handle);
        }
    }
    textures_.clear();
    images_.clear();
    surface_ = {};
    completed_fence_value_ = 0U;
    last_submitted_fence_value_ = 0U;
    has_latest_receipt_ = false;
    latest_receipt_ = {};
    if (++config_.device_generation == 0U) {
        config_.device_generation = 0U;
    }
    if (error != nullptr) {
        error->clear();
    }
    return config_.device_generation != 0U;
}

bool GpuDevicePresentationBackend::latest_present_receipt(
    GpuPresentReceipt* output) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_latest_receipt_ || output == nullptr) {
        return false;
    }
    *output = latest_receipt_;
    return true;
}

GpuDevicePresentationSnapshot
GpuDevicePresentationBackend::snapshot_locked() const noexcept {
    GpuDevicePresentationSnapshot snapshot;
    snapshot.metadata = ledger_.snapshot(
        core::ResourceClass::CompositorSurface);
    snapshot.config = config_;
    snapshot.surface = surface_;
    snapshot.completed_fence_value = completed_fence_value_;
    snapshot.last_submitted_fence_value = last_submitted_fence_value_;
    snapshot.upload_submissions = upload_submissions_;
    snapshot.present_submissions = present_submissions_;
    snapshot.retired_frames = retired_frames_;
    snapshot.texture_allocations = texture_allocations_;
    snapshot.texture_reuses = texture_reuses_;
    snapshot.texture_evictions = texture_evictions_;
    snapshot.surface_reconfigurations = surface_reconfigurations_;
    snapshot.stale_rejections = stale_rejections_;
    snapshot.texture_count = textures_.size();
    snapshot.surface_image_count = images_.size();
    snapshot.in_flight_frame_count = count_in_flight(frames_);
    snapshot.texture_pin_count = pins_.size();
    return snapshot;
}

GpuDevicePresentationSnapshot
GpuDevicePresentationBackend::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_locked();
}

} // namespace zevryon::text
