#include "gpu_compositor_submission.hpp"

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

std::pmr::memory_resource* usable_resource(
    std::pmr::memory_resource* resource) noexcept {
    return resource != nullptr ? resource : std::pmr::get_default_resource();
}

void clear_backend_error(GpuCompositorBackendError* error) noexcept {
    if (error != nullptr) {
        error->kind = GpuCompositorBackendErrorKind::None;
        error->message.clear();
    }
}

bool fail_backend(
    GpuCompositorBackendErrorKind kind,
    const char* message,
    GpuCompositorBackendError* error) noexcept {
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

void clear_error(GpuCompositorFrameError* error) noexcept {
    if (error != nullptr) {
        error->kind = GpuCompositorFrameErrorKind::None;
        error->command_index = 0U;
        error->upload_index = 0U;
        error->draw_index = 0U;
        error->page_index = 0U;
        clear_backend_error(&error->backend_error);
        error->message.clear();
    }
}

bool fail(
    GpuCompositorFrameErrorKind kind,
    std::size_t command_index,
    std::size_t upload_index,
    std::size_t draw_index,
    std::uint32_t page_index,
    const char* message,
    const GpuCompositorBackendError* backend_error,
    GpuCompositorFrameError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->command_index = command_index;
        error->upload_index = upload_index;
        error->draw_index = draw_index;
        error->page_index = page_index;
        if (backend_error != nullptr) {
            error->backend_error = *backend_error;
        }
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool texture_matches(
    const GpuTextureResidencyRecord& record,
    std::uint64_t device_generation,
    std::uint64_t atlas_generation_id,
    std::uint64_t page_generation,
    std::uint32_t page_index) noexcept {
    return record.handle.device_generation == device_generation &&
        record.atlas_generation_id == atlas_generation_id &&
        record.page_generation == page_generation &&
        record.handle.page_index == page_index &&
        (record.handle.flags & kGpuTextureHandleValid) != 0U;
}

GpuTextureResidencyRecord* find_texture(
    std::pmr::vector<GpuTextureResidencyRecord>* textures,
    std::uint64_t device_generation,
    std::uint64_t atlas_generation_id,
    std::uint64_t page_generation,
    std::uint32_t page_index) noexcept {
    const auto it = std::find_if(
        textures->begin(),
        textures->end(),
        [=](const GpuTextureResidencyRecord& record) {
            return texture_matches(
                record,
                device_generation,
                atlas_generation_id,
                page_generation,
                page_index);
        });
    return it == textures->end() ? nullptr : &*it;
}

bool texture_required_by_frame(
    const GpuTextureResidencyRecord& texture,
    const GpuCompositorFrameRequest& request) noexcept {
    const bool required_by_upload = std::any_of(
        request.upload_execution->batches.begin(),
        request.upload_execution->batches.end(),
        [&](const GlyphAtlasBackendUploadBatch& batch) {
            return texture.atlas_generation_id == batch.atlas_generation_id &&
                texture.page_generation == batch.page_generation &&
                texture.handle.page_index == batch.page_index;
        });
    if (required_by_upload) {
        return true;
    }
    return std::any_of(
        request.atlas_submission->draw_batches.begin(),
        request.atlas_submission->draw_batches.end(),
        [&](const GlyphAtlasDrawBatch& batch) {
            return texture.atlas_generation_id ==
                    request.atlas_submission->atlas_generation_id &&
                texture.page_generation == batch.page_generation &&
                texture.handle.page_index == batch.page_index;
        });
}

bool append_fill_packet(
    const TextPaintFillRect& source,
    std::pmr::vector<GpuFillRectPacket>* fills) {
    GpuFillRectPacket packet;
    packet.viewport_inline_start = source.viewport_inline_start;
    packet.viewport_block_start = source.viewport_block_start;
    packet.inline_size = source.inline_size;
    packet.block_size = source.block_size;
    packet.style_id = source.style_id;
    packet.source_line_index = source.source_line_index;
    packet.source_fragment_index = source.source_fragment_index;
    packet.flags = source.flags;
    fills->push_back(packet);
    return true;
}

} // namespace

GpuCompositorFrame::GpuCompositorFrame(std::pmr::memory_resource* resource)
    : clips(usable_resource(resource)),
      texture_uploads(usable_resource(resource)),
      glyph_draws(usable_resource(resource)),
      fill_rects(usable_resource(resource)),
      commands(usable_resource(resource)) {}

std::pmr::memory_resource* GpuCompositorFrame::resource() const noexcept {
    return commands.get_allocator().resource();
}

void GpuCompositorFrame::release() noexcept {
    device_generation = 0U;
    frame_generation = 0U;
    atlas_generation_id = 0U;
    required_upload_fence = 0U;
    release_vector(&clips);
    release_vector(&texture_uploads);
    release_vector(&glyph_draws);
    release_vector(&fill_rects);
    release_vector(&commands);
}

bool ReferenceGpuCompositorBackend::allocate_texture(
    std::uint32_t page_index,
    GlyphRasterFormat format,
    std::uint32_t width,
    std::uint32_t height,
    std::uint64_t device_generation,
    GpuTextureHandle* output,
    GpuCompositorBackendError* error) noexcept {
    clear_backend_error(error);
    if (output == nullptr || width == 0U || height == 0U ||
        device_generation == 0U || format == GlyphRasterFormat::Empty ||
        next_texture_id_ == std::numeric_limits<std::uint64_t>::max() ||
        next_texture_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail_backend(
            GpuCompositorBackendErrorKind::InvalidInput,
            "invalid reference GPU texture allocation",
            error);
    }
    *output = {};
    output->device_generation = device_generation;
    output->texture_generation = next_texture_generation_++;
    output->texture_id = next_texture_id_++;
    output->page_index = page_index;
    output->format = format;
    output->flags = kGpuTextureHandleValid;
    return true;
}

void ReferenceGpuCompositorBackend::release_texture(
    const GpuTextureHandle&) noexcept {}

bool ReferenceGpuCompositorBackend::encode_uploads(
    const GpuTextureUploadCommand& command,
    std::span<const GlyphAtlasUploadRecord> uploads,
    std::span<const std::byte> payload,
    GpuCompositorBackendError* error) noexcept {
    clear_backend_error(error);
    if ((command.texture.flags & kGpuTextureHandleValid) == 0U ||
        uploads.empty() || command.upload_count != uploads.size()) {
        return fail_backend(
            GpuCompositorBackendErrorKind::InvalidInput,
            "invalid reference GPU upload command",
            error);
    }
    for (const GlyphAtlasUploadRecord& upload : uploads) {
        if (upload.page_index != command.texture.page_index ||
            upload.page_generation != command.page_generation ||
            upload.atlas_generation_id != command.atlas_generation_id ||
            upload.format != command.texture.format ||
            upload.payload_offset > payload.size() ||
            upload.payload_size > payload.size() - upload.payload_offset) {
            return fail_backend(
                GpuCompositorBackendErrorKind::UploadFailed,
                "GPU upload payload or texture topology mismatch",
                error);
        }
    }
    return true;
}

bool ReferenceGpuCompositorBackend::submit_frame(
    const GpuCompositorFrame& frame,
    std::uint64_t frame_id,
    std::uint64_t* fence_value,
    GpuCompositorBackendError* error) noexcept {
    clear_backend_error(error);
    if (fence_value == nullptr || frame_id == 0U ||
        frame.device_generation == 0U || frame.frame_generation == 0U ||
        next_fence_value_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail_backend(
            GpuCompositorBackendErrorKind::InvalidInput,
            "invalid reference GPU frame submission",
            error);
    }
    if (frame.required_upload_fence ==
        std::numeric_limits<std::uint64_t>::max()) {
        return fail_backend(
            GpuCompositorBackendErrorKind::FenceOverflow,
            "reference GPU required fence overflow",
            error);
    }
    next_fence_value_ = std::max(
        next_fence_value_,
        frame.required_upload_fence + 1U);
    if (next_fence_value_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail_backend(
            GpuCompositorBackendErrorKind::FenceOverflow,
            "reference GPU frame fence overflow",
            error);
    }
    *fence_value = next_fence_value_++;
    return true;
}

GpuTextureResidencyCache::GpuTextureResidencyCache(
    GpuTextureConfig config,
    std::size_t metadata_hard_limit) noexcept
    : metadata_resource_(
          ledger_,
          core::ResourceClass::CompositorSurface,
          std::pmr::new_delete_resource()),
      textures_(&metadata_resource_),
      in_flight_(&metadata_resource_),
      config_(config) {
    ledger_.set_hard_limit(
        core::ResourceClass::CompositorSurface,
        metadata_hard_limit);
    try {
        textures_.reserve(config_.maximum_textures);
        in_flight_.resize(config_.maximum_in_flight_frames);
    } catch (...) {
        textures_.clear();
        in_flight_.clear();
    }
}

void GpuTextureResidencyCache::clear(GpuCompositorBackend* backend) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (backend != nullptr) {
        for (const GpuTextureResidencyRecord& record : textures_) {
            if ((record.handle.flags & kGpuTextureHandleValid) != 0U) {
                backend->release_texture(record.handle);
                ++released_textures_;
            }
        }
    }
    textures_.clear();
    for (GpuInFlightFrameRecord& frame : in_flight_) {
        frame = {};
    }
    ++residency_epoch_;
    if (config_.device_generation == std::numeric_limits<std::uint64_t>::max()) {
        config_.device_generation = 0U;
    } else {
        ++config_.device_generation;
    }
}

void GpuTextureResidencyCache::retire_completed_frames(
    std::uint64_t completed_fence) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (GpuInFlightFrameRecord& frame : in_flight_) {
        if (frame.occupied != 0U && frame.receipt.fence_value <= completed_fence) {
            frame = {};
            ++retired_frames_;
        }
    }
    for (GpuTextureResidencyRecord& texture : textures_) {
        if (texture.state == GpuTextureResidencyState::Pending &&
            texture.ready_fence_value != 0U &&
            texture.ready_fence_value <= completed_fence) {
            texture.state = GpuTextureResidencyState::Resident;
        }
    }
}

GpuTextureResidencyStats GpuTextureResidencyCache::snapshot_locked() const noexcept {
    GpuTextureResidencyStats result;
    result.metadata = ledger_.snapshot(core::ResourceClass::CompositorSurface);
    result.config = config_;
    result.residency_epoch = residency_epoch_;
    result.next_frame_id = next_frame_id_;
    result.allocated_textures = allocated_textures_;
    result.reused_textures = reused_textures_;
    result.released_textures = released_textures_;
    result.evicted_textures = evicted_textures_;
    result.submitted_frames = submitted_frames_;
    result.retired_frames = retired_frames_;
    result.stale_rejections = stale_rejections_;
    result.texture_count = textures_.size();
    result.in_flight_count = static_cast<std::size_t>(std::count_if(
        in_flight_.begin(),
        in_flight_.end(),
        [](const GpuInFlightFrameRecord& frame) {
            return frame.occupied != 0U;
        }));
    return result;
}

GpuTextureResidencyStats GpuTextureResidencyCache::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_locked();
}

const char* gpu_compositor_frame_error_kind_name(
    GpuCompositorFrameErrorKind kind) noexcept {
    switch (kind) {
        case GpuCompositorFrameErrorKind::None: return "none";
        case GpuCompositorFrameErrorKind::InvalidInput: return "invalid-input";
        case GpuCompositorFrameErrorKind::StaleAtlasSubmission: return "stale-atlas-submission";
        case GpuCompositorFrameErrorKind::StaleUploadExecution: return "stale-upload-execution";
        case GpuCompositorFrameErrorKind::TextureCapacityExceeded: return "texture-capacity-exceeded";
        case GpuCompositorFrameErrorKind::InFlightCapacityExceeded: return "in-flight-capacity-exceeded";
        case GpuCompositorFrameErrorKind::MissingTextureResidency: return "missing-texture-residency";
        case GpuCompositorFrameErrorKind::UploadTopologyViolation: return "upload-topology-violation";
        case GpuCompositorFrameErrorKind::CommandTopologyViolation: return "command-topology-violation";
        case GpuCompositorFrameErrorKind::ArithmeticOverflow: return "arithmetic-overflow";
        case GpuCompositorFrameErrorKind::FrameLimitExceeded: return "frame-limit-exceeded";
        case GpuCompositorFrameErrorKind::MetadataBudgetExceeded: return "metadata-budget-exceeded";
        case GpuCompositorFrameErrorKind::OutputBudgetExceeded: return "output-budget-exceeded";
        case GpuCompositorFrameErrorKind::BackendFailure: return "backend-failure";
        case GpuCompositorFrameErrorKind::AggregateOverflow: return "aggregate-overflow";
    }
    return "unknown";
}

bool prepare_gpu_compositor_frame(
    const GpuCompositorFrameRequest& request,
    GpuTextureResidencyCache* cache,
    GpuCompositorBackend* backend,
    GpuCompositorFrame* output,
    GpuCompositorFrameStats* stats,
    GpuCompositorFrameError* error) noexcept {
    clear_error(error);
    if (output == nullptr || cache == nullptr || backend == nullptr) {
        return fail(
            GpuCompositorFrameErrorKind::InvalidInput,
            0U, 0U, 0U, 0U,
            "GPU compositor frame requires output, cache and backend",
            nullptr,
            error);
    }
    output->release();
    if (stats != nullptr) {
        *stats = {};
    }
    if (request.paint_stream == nullptr || request.atlas_submission == nullptr ||
        request.upload_execution == nullptr || request.atlas_cache == nullptr ||
        request.frame_generation == 0U || request.limits.maximum_commands == 0U ||
        request.limits.maximum_glyph_draws == 0U) {
        return fail(
            GpuCompositorFrameErrorKind::InvalidInput,
            0U, 0U, 0U, 0U,
            "invalid GPU compositor frame request",
            nullptr,
            error);
    }
    if (!glyph_atlas_submission_is_current(
            *request.atlas_cache,
            *request.atlas_submission)) {
        return fail(
            GpuCompositorFrameErrorKind::StaleAtlasSubmission,
            0U, 0U, 0U, 0U,
            "atlas submission is stale",
            nullptr,
            error);
    }
    if (!glyph_atlas_upload_execution_is_current(
            *request.atlas_cache,
            *request.atlas_submission,
            *request.upload_execution)) {
        return fail(
            GpuCompositorFrameErrorKind::StaleUploadExecution,
            0U, 0U, 0U, 0U,
            "atlas upload execution is stale",
            nullptr,
            error);
    }

    std::size_t selection_command_count = 0U;
    std::size_t caret_command_count = 0U;
    for (const TextPaintCommandRecord& command : request.paint_stream->commands) {
        if (command.kind == TextPaintCommandKind::SelectionRect) {
            ++selection_command_count;
        } else if (command.kind == TextPaintCommandKind::CaretRect) {
            ++caret_command_count;
        }
    }
    const std::size_t fill_count =
        selection_command_count + caret_command_count;
    const std::size_t compositor_command_count =
        selection_command_count +
        request.atlas_submission->draw_batches.size() +
        caret_command_count;
    if (request.upload_execution->batches.size() >
            request.limits.maximum_texture_uploads ||
        request.atlas_submission->draw_batches.size() >
            request.limits.maximum_glyph_draws ||
        fill_count > request.limits.maximum_fill_rects ||
        compositor_command_count > request.limits.maximum_commands) {
        return fail(
            GpuCompositorFrameErrorKind::FrameLimitExceeded,
            compositor_command_count, 0U, 0U, 0U,
            "GPU compositor request exceeds fixed output limits",
            nullptr,
            error);
    }

    std::lock_guard<std::mutex> lock(cache->mutex_);
    if (cache->config_.device_generation == 0U ||
        cache->config_.page_width == 0U || cache->config_.page_height == 0U ||
        cache->config_.maximum_textures == 0U ||
        cache->config_.maximum_in_flight_frames == 0U) {
        return fail(
            GpuCompositorFrameErrorKind::InvalidInput,
            0U, 0U, 0U, 0U,
            "invalid GPU texture cache configuration",
            nullptr,
            error);
    }
    if (stats != nullptr) {
        stats->metadata_before = cache->ledger_.snapshot(
            core::ResourceClass::CompositorSurface);
        stats->input_paint_commands = request.paint_stream->commands.size();
        stats->input_uploads = request.atlas_submission->uploads.size();
        stats->input_draw_batches = request.atlas_submission->draw_batches.size();
    }

    std::vector<GpuTextureHandle> newly_allocated;
    std::vector<GpuTextureHandle> evicted_handles;
    try {
        newly_allocated.reserve(cache->config_.maximum_textures);
        evicted_handles.reserve(cache->config_.maximum_textures);
        std::pmr::vector<GpuTextureResidencyRecord> staged_textures(
            &cache->metadata_resource_);
        staged_textures.reserve(cache->config_.maximum_textures);
        staged_textures = cache->textures_;
        for (GpuTextureResidencyRecord& texture : staged_textures) {
            if (texture.state == GpuTextureResidencyState::Pending &&
                texture.ready_fence_value != 0U &&
                texture.ready_fence_value <= request.completed_upload_fence) {
                texture.state = GpuTextureResidencyState::Resident;
            }
        }

        GpuCompositorFrame working(output->resource());
        std::pmr::vector<std::uint32_t> caret_fill_indices(output->resource());
        working.device_generation = cache->config_.device_generation;
        working.frame_generation = request.frame_generation;
        working.atlas_generation_id = request.atlas_submission->atlas_generation_id;
        working.clips.reserve(request.paint_stream->clips.size());
        working.texture_uploads.reserve(request.upload_execution->batches.size());
        working.glyph_draws.reserve(request.atlas_submission->draw_batches.size());
        working.fill_rects.reserve(fill_count);
        working.commands.reserve(compositor_command_count);
        caret_fill_indices.reserve(caret_command_count);
        working.clips = request.paint_stream->clips;

        for (std::size_t batch_index = 0U;
             batch_index < request.upload_execution->batches.size();
             ++batch_index) {
            const GlyphAtlasBackendUploadBatch& batch =
                request.upload_execution->batches[batch_index];
            const GlyphAtlasUploadReceipt& receipt =
                request.upload_execution->receipts[batch_index];
            if (batch.upload_count == 0U ||
                batch.first_upload > request.atlas_submission->uploads.size() ||
                batch.upload_count >
                    request.atlas_submission->uploads.size() - batch.first_upload ||
                receipt.status != GlyphAtlasUploadReceiptStatus::Completed ||
                receipt.first_upload != batch.first_upload ||
                receipt.upload_count != batch.upload_count ||
                receipt.page_index != batch.page_index ||
                receipt.page_generation != batch.page_generation ||
                receipt.atlas_generation_id != batch.atlas_generation_id ||
                receipt.fence_value == 0U) {
                for (const GpuTextureHandle& handle : newly_allocated) {
                    backend->release_texture(handle);
                }
                return fail(
                    GpuCompositorFrameErrorKind::UploadTopologyViolation,
                    0U, batch.first_upload, 0U, batch.page_index,
                    "GPU upload batch and receipt topology mismatch",
                    nullptr,
                    error);
            }
            bool newly_created = false;
            GpuTextureResidencyRecord* texture = find_texture(
                &staged_textures,
                cache->config_.device_generation,
                batch.atlas_generation_id,
                batch.page_generation,
                batch.page_index);
            if (texture == nullptr) {
                if (staged_textures.size() >= cache->config_.maximum_textures) {
                    const bool has_in_flight = std::any_of(
                        cache->in_flight_.begin(),
                        cache->in_flight_.end(),
                        [](const GpuInFlightFrameRecord& frame) {
                            return frame.occupied != 0U;
                        });
                    auto victim = staged_textures.end();
                    for (auto candidate = staged_textures.begin();
                         candidate != staged_textures.end();
                         ++candidate) {
                        if (texture_required_by_frame(*candidate, request)) {
                            continue;
                        }
                        if (victim == staged_textures.end() ||
                            candidate->last_use_epoch < victim->last_use_epoch) {
                            victim = candidate;
                        }
                    }
                    if (has_in_flight || victim == staged_textures.end() ||
                        victim->last_use_epoch > cache->residency_epoch_) {
                        for (const GpuTextureHandle& handle : newly_allocated) {
                            backend->release_texture(handle);
                        }
                        return fail(
                            GpuCompositorFrameErrorKind::TextureCapacityExceeded,
                            0U, batch.first_upload, 0U, batch.page_index,
                            "GPU texture residency has no safely evictable page",
                            nullptr,
                            error);
                    }
                    evicted_handles.push_back(victim->handle);
                    staged_textures.erase(victim);
                    if (stats != nullptr) {
                        ++stats->evicted_textures;
                    }
                }
                GpuTextureHandle handle;
                GpuCompositorBackendError backend_error;
                if (!backend->allocate_texture(
                        batch.page_index,
                        batch.format,
                        cache->config_.page_width,
                        cache->config_.page_height,
                        cache->config_.device_generation,
                        &handle,
                        &backend_error)) {
                    for (const GpuTextureHandle& allocated : newly_allocated) {
                        backend->release_texture(allocated);
                    }
                    return fail(
                        GpuCompositorFrameErrorKind::BackendFailure,
                        0U, batch.first_upload, 0U, batch.page_index,
                        "GPU texture allocation failed",
                        &backend_error,
                        error);
                }
                newly_allocated.push_back(handle);
                GpuTextureResidencyRecord record;
                record.handle = handle;
                record.atlas_generation_id = batch.atlas_generation_id;
                record.page_generation = batch.page_generation;
                record.last_use_epoch = cache->residency_epoch_ + 1U;
                record.ready_fence_value = 0U;
                record.state = GpuTextureResidencyState::Pending;
                staged_textures.push_back(record);
                texture = &staged_textures.back();
                newly_created = true;
                if (stats != nullptr) {
                    ++stats->allocated_textures;
                }
            } else {
                if (texture->last_use_epoch != cache->residency_epoch_ + 1U &&
                    stats != nullptr) {
                    ++stats->reused_textures;
                }
                texture->last_use_epoch = cache->residency_epoch_ + 1U;
                if (texture->state == GpuTextureResidencyState::Pending &&
                    texture->ready_fence_value != 0U &&
                    texture->ready_fence_value <= request.completed_upload_fence) {
                    texture->state = GpuTextureResidencyState::Resident;
                }
            }

            const bool needs_upload = newly_created ||
                (texture->state == GpuTextureResidencyState::Pending &&
                 texture->ready_fence_value == 0U);
            if (!needs_upload) {
                continue;
            }
            if (working.texture_uploads.size() >=
                request.limits.maximum_texture_uploads) {
                for (const GpuTextureHandle& handle : newly_allocated) {
                    backend->release_texture(handle);
                }
                return fail(
                    GpuCompositorFrameErrorKind::FrameLimitExceeded,
                    0U, batch.first_upload, 0U, batch.page_index,
                    "GPU texture upload command limit exceeded",
                    nullptr,
                    error);
            }
            GpuTextureUploadCommand command;
            command.texture = texture->handle;
            command.atlas_generation_id = batch.atlas_generation_id;
            command.page_generation = batch.page_generation;
            command.required_fence_value = receipt.fence_value;
            command.first_upload = batch.first_upload;
            command.upload_count = batch.upload_count;
            command.flags = kGpuTextureUploadRequiresWait;
            GpuCompositorBackendError backend_error;
            const auto uploads = std::span<const GlyphAtlasUploadRecord>(
                request.atlas_submission->uploads.data(),
                request.atlas_submission->uploads.size()).subspan(
                    batch.first_upload,
                    batch.upload_count);
            if (!backend->encode_uploads(
                    command,
                    uploads,
                    request.raster_payload,
                    &backend_error)) {
                for (const GpuTextureHandle& handle : newly_allocated) {
                    backend->release_texture(handle);
                }
                return fail(
                    GpuCompositorFrameErrorKind::BackendFailure,
                    0U, batch.first_upload, 0U, batch.page_index,
                    "GPU upload encoding failed",
                    &backend_error,
                    error);
            }
            working.texture_uploads.push_back(command);
        }

        std::size_t selection_count = 0U;
        std::size_t caret_count = 0U;
        for (std::size_t command_index = 0U;
             command_index < request.paint_stream->commands.size();
             ++command_index) {
            const TextPaintCommandRecord& command =
                request.paint_stream->commands[command_index];
            if (command.kind == TextPaintCommandKind::SelectionRect ||
                command.kind == TextPaintCommandKind::CaretRect) {
                if (command.payload_index >= request.paint_stream->fill_rects.size() ||
                    command.clip_index >= request.paint_stream->clips.size()) {
                    for (const GpuTextureHandle& handle : newly_allocated) {
                        backend->release_texture(handle);
                    }
                    return fail(
                        GpuCompositorFrameErrorKind::CommandTopologyViolation,
                        command_index, 0U, 0U, 0U,
                        "paint fill command references invalid payload or clip",
                        nullptr,
                        error);
                }
                if (working.fill_rects.size() >= request.limits.maximum_fill_rects ||
                    working.commands.size() >= request.limits.maximum_commands) {
                    for (const GpuTextureHandle& handle : newly_allocated) {
                        backend->release_texture(handle);
                    }
                    return fail(
                        GpuCompositorFrameErrorKind::FrameLimitExceeded,
                        command_index, 0U, 0U, 0U,
                        "GPU fill or command limit exceeded",
                        nullptr,
                        error);
                }
                const std::uint32_t fill_index = static_cast<std::uint32_t>(
                    working.fill_rects.size());
                append_fill_packet(
                    request.paint_stream->fill_rects[command.payload_index],
                    &working.fill_rects);
                if (command.kind == TextPaintCommandKind::SelectionRect) {
                    working.commands.push_back({
                        GpuCompositorCommandKind::SelectionFill,
                        fill_index,
                        command.clip_index,
                        command.flags});
                    ++selection_count;
                } else {
                    caret_fill_indices.push_back(fill_index);
                    ++caret_count;
                }
            }
        }

        for (std::size_t draw_index = 0U;
             draw_index < request.atlas_submission->draw_batches.size();
             ++draw_index) {
            const GlyphAtlasDrawBatch& draw =
                request.atlas_submission->draw_batches[draw_index];
            if (draw.instance_count == 0U ||
                draw.first_instance > request.atlas_submission->draw_instances.size() ||
                draw.instance_count >
                    request.atlas_submission->draw_instances.size() - draw.first_instance ||
                draw.clip_index >= request.paint_stream->clips.size()) {
                for (const GpuTextureHandle& handle : newly_allocated) {
                    backend->release_texture(handle);
                }
                return fail(
                    GpuCompositorFrameErrorKind::CommandTopologyViolation,
                    0U, 0U, draw_index, draw.page_index,
                    "atlas draw batch references invalid instance or clip",
                    nullptr,
                    error);
            }
            GpuTextureResidencyRecord* texture = find_texture(
                &staged_textures,
                cache->config_.device_generation,
                request.atlas_submission->atlas_generation_id,
                draw.page_generation,
                draw.page_index);
            if (texture == nullptr) {
                for (const GpuTextureHandle& handle : newly_allocated) {
                    backend->release_texture(handle);
                }
                return fail(
                    GpuCompositorFrameErrorKind::MissingTextureResidency,
                    0U, 0U, draw_index, draw.page_index,
                    "draw batch has no matching GPU texture residency",
                    nullptr,
                    error);
            }
            if (texture->last_use_epoch != cache->residency_epoch_ + 1U &&
                stats != nullptr) {
                ++stats->reused_textures;
            }
            texture->last_use_epoch = cache->residency_epoch_ + 1U;
            if (working.glyph_draws.size() >= request.limits.maximum_glyph_draws ||
                working.commands.size() >= request.limits.maximum_commands) {
                for (const GpuTextureHandle& handle : newly_allocated) {
                    backend->release_texture(handle);
                }
                return fail(
                    GpuCompositorFrameErrorKind::FrameLimitExceeded,
                    0U, 0U, draw_index, draw.page_index,
                    "GPU draw or command limit exceeded",
                    nullptr,
                    error);
            }
            GpuGlyphDrawPacket packet;
            packet.texture = texture->handle;
            packet.required_fence_value = texture->ready_fence_value;
            packet.first_instance = draw.first_instance;
            packet.instance_count = draw.instance_count;
            packet.style_id = draw.style_id;
            packet.clip_index = draw.clip_index;
            if (texture->state == GpuTextureResidencyState::Pending) {
                packet.flags |= kGpuGlyphDrawRequiresUploadWait;
            }
            if ((draw.flags & kGlyphAtlasDrawBatchCoalesced) != 0U) {
                packet.flags |= kGpuGlyphDrawCoalesced;
            }
            const std::uint32_t packet_index = static_cast<std::uint32_t>(
                working.glyph_draws.size());
            working.glyph_draws.push_back(packet);
            working.commands.push_back({
                GpuCompositorCommandKind::GlyphDraw,
                packet_index,
                draw.clip_index,
                0U});
            working.required_upload_fence = std::max(
                working.required_upload_fence,
                texture->ready_fence_value);
            if (stats != nullptr) {
                stats->maximum_instances_per_draw = std::max(
                    stats->maximum_instances_per_draw,
                    static_cast<std::uint64_t>(draw.instance_count));
            }
        }

        std::size_t caret_index = 0U;
        for (std::size_t command_index = 0U;
             command_index < request.paint_stream->commands.size();
             ++command_index) {
            const TextPaintCommandRecord& command =
                request.paint_stream->commands[command_index];
            if (command.kind != TextPaintCommandKind::CaretRect) {
                continue;
            }
            if (caret_index >= caret_fill_indices.size() ||
                working.commands.size() >= request.limits.maximum_commands) {
                for (const GpuTextureHandle& handle : newly_allocated) {
                    backend->release_texture(handle);
                }
                return fail(
                    GpuCompositorFrameErrorKind::FrameLimitExceeded,
                    command_index, 0U, 0U, 0U,
                    "GPU caret command limit exceeded",
                    nullptr,
                    error);
            }
            working.commands.push_back({
                GpuCompositorCommandKind::CaretFill,
                caret_fill_indices[caret_index],
                command.clip_index,
                command.flags});
            ++caret_index;
        }

        if (working.commands.size() > request.limits.maximum_commands) {
            for (const GpuTextureHandle& handle : newly_allocated) {
                backend->release_texture(handle);
            }
            return fail(
                GpuCompositorFrameErrorKind::FrameLimitExceeded,
                working.commands.size(), 0U, 0U, 0U,
                "GPU compositor command limit exceeded",
                nullptr,
                error);
        }

        cache->textures_.swap(staged_textures);
        for (const GpuTextureHandle& handle : evicted_handles) {
            backend->release_texture(handle);
        }
        cache->residency_epoch_++;
        cache->allocated_textures_ += newly_allocated.size();
        cache->evicted_textures_ += evicted_handles.size();
        cache->released_textures_ += evicted_handles.size();
        cache->reused_textures_ += stats != nullptr ? stats->reused_textures : 0U;
        output->device_generation = working.device_generation;
        output->frame_generation = working.frame_generation;
        output->atlas_generation_id = working.atlas_generation_id;
        output->required_upload_fence = working.required_upload_fence;
        output->clips.swap(working.clips);
        output->texture_uploads.swap(working.texture_uploads);
        output->glyph_draws.swap(working.glyph_draws);
        output->fill_rects.swap(working.fill_rects);
        output->commands.swap(working.commands);

        if (stats != nullptr) {
            stats->metadata_after = cache->ledger_.snapshot(
                core::ResourceClass::CompositorSurface);
            stats->output_upload_commands = output->texture_uploads.size();
            stats->output_glyph_draws = output->glyph_draws.size();
            stats->output_fill_rects = output->fill_rects.size();
            stats->output_commands = output->commands.size();
            stats->selection_commands = selection_count;
            stats->caret_commands = caret_count;
            stats->required_upload_fence = output->required_upload_fence;
            for (const GpuTextureResidencyRecord& texture : cache->textures_) {
                if (texture.state == GpuTextureResidencyState::Pending) {
                    ++stats->pending_textures;
                } else if (texture.state == GpuTextureResidencyState::Resident) {
                    ++stats->resident_textures;
                }
            }
        }
        return true;
    } catch (const std::bad_alloc&) {
        for (const GpuTextureHandle& handle : newly_allocated) {
            backend->release_texture(handle);
        }
        return fail(
            GpuCompositorFrameErrorKind::OutputBudgetExceeded,
            0U, 0U, 0U, 0U,
            "GPU compositor output or metadata budget exceeded",
            nullptr,
            error);
    } catch (...) {
        for (const GpuTextureHandle& handle : newly_allocated) {
            backend->release_texture(handle);
        }
        return fail(
            GpuCompositorFrameErrorKind::AggregateOverflow,
            0U, 0U, 0U, 0U,
            "unexpected GPU compositor frame failure",
            nullptr,
            error);
    }
}

bool submit_gpu_compositor_frame(
    const GpuCompositorFrame& frame,
    GpuTextureResidencyCache* cache,
    GpuCompositorBackend* backend,
    GpuFrameReceipt* receipt,
    GpuCompositorFrameError* error) noexcept {
    clear_error(error);
    if (cache == nullptr || backend == nullptr || receipt == nullptr ||
        frame.device_generation == 0U || frame.frame_generation == 0U) {
        return fail(
            GpuCompositorFrameErrorKind::InvalidInput,
            0U, 0U, 0U, 0U,
            "invalid GPU frame submission request",
            nullptr,
            error);
    }
    *receipt = {};
    std::lock_guard<std::mutex> lock(cache->mutex_);
    if (frame.device_generation != cache->config_.device_generation) {
        ++cache->stale_rejections_;
        return fail(
            GpuCompositorFrameErrorKind::CommandTopologyViolation,
            0U, 0U, 0U, 0U,
            "GPU frame device generation is stale",
            nullptr,
            error);
    }
    const auto free_it = std::find_if(
        cache->in_flight_.begin(),
        cache->in_flight_.end(),
        [](const GpuInFlightFrameRecord& record) {
            return record.occupied == 0U;
        });
    if (free_it == cache->in_flight_.end()) {
        return fail(
            GpuCompositorFrameErrorKind::InFlightCapacityExceeded,
            0U, 0U, 0U, 0U,
            "GPU in-flight frame ring is full",
            nullptr,
            error);
    }
    if (cache->next_frame_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail(
            GpuCompositorFrameErrorKind::ArithmeticOverflow,
            0U, 0U, 0U, 0U,
            "GPU frame ID overflow",
            nullptr,
            error);
    }
    for (const GpuGlyphDrawPacket& draw : frame.glyph_draws) {
        const auto texture = std::find_if(
            cache->textures_.begin(),
            cache->textures_.end(),
            [&](const GpuTextureResidencyRecord& candidate) {
                return candidate.handle == draw.texture &&
                    candidate.atlas_generation_id == frame.atlas_generation_id;
            });
        if (texture == cache->textures_.end()) {
            ++cache->stale_rejections_;
            return fail(
                GpuCompositorFrameErrorKind::MissingTextureResidency,
                0U, 0U, 0U, draw.texture.page_index,
                "GPU draw packet texture became stale before submission",
                nullptr,
                error);
        }
    }
    for (const GpuTextureUploadCommand& upload : frame.texture_uploads) {
        const auto texture = std::find_if(
            cache->textures_.begin(),
            cache->textures_.end(),
            [&](const GpuTextureResidencyRecord& candidate) {
                return candidate.handle == upload.texture &&
                    candidate.atlas_generation_id == frame.atlas_generation_id;
            });
        if (texture == cache->textures_.end()) {
            ++cache->stale_rejections_;
            return fail(
                GpuCompositorFrameErrorKind::MissingTextureResidency,
                0U, upload.first_upload, 0U, upload.texture.page_index,
                "GPU upload command texture became stale before submission",
                nullptr,
                error);
        }
    }
    const std::uint64_t frame_id = cache->next_frame_id_;
    std::uint64_t fence_value = 0U;
    GpuCompositorBackendError backend_error;
    if (!backend->submit_frame(
            frame,
            frame_id,
            &fence_value,
            &backend_error) || fence_value == 0U) {
        return fail(
            GpuCompositorFrameErrorKind::BackendFailure,
            0U, 0U, 0U, 0U,
            "GPU frame backend submission failed",
            &backend_error,
            error);
    }
    for (const GpuTextureUploadCommand& upload : frame.texture_uploads) {
        const auto texture = std::find_if(
            cache->textures_.begin(),
            cache->textures_.end(),
            [&](const GpuTextureResidencyRecord& candidate) {
                return candidate.handle == upload.texture &&
                    candidate.atlas_generation_id == frame.atlas_generation_id;
            });
        if (texture == cache->textures_.end()) {
            continue;
        }
        texture->ready_fence_value = fence_value;
        texture->state = GpuTextureResidencyState::Pending;
    }
    const std::size_t slot_index = static_cast<std::size_t>(
        std::distance(cache->in_flight_.begin(), free_it));
    GpuFrameReceipt next;
    next.frame_id = frame_id;
    next.frame_generation = frame.frame_generation;
    next.device_generation = frame.device_generation;
    next.fence_value = fence_value;
    next.required_upload_fence = frame.required_upload_fence;
    next.slot_index = static_cast<std::uint32_t>(slot_index);
    free_it->receipt = next;
    free_it->atlas_generation_id = frame.atlas_generation_id;
    free_it->occupied = 1U;
    ++cache->next_frame_id_;
    ++cache->submitted_frames_;
    *receipt = next;
    return true;
}

bool gpu_compositor_frame_is_current(
    const GpuTextureResidencyCache& cache,
    const GpuCompositorFrame& frame) noexcept {
    std::lock_guard<std::mutex> lock(cache.mutex_);
    if (frame.device_generation == 0U ||
        frame.device_generation != cache.config_.device_generation ||
        frame.atlas_generation_id == 0U) {
        return false;
    }
    for (const GpuGlyphDrawPacket& draw : frame.glyph_draws) {
        const auto it = std::find_if(
            cache.textures_.begin(),
            cache.textures_.end(),
            [&](const GpuTextureResidencyRecord& candidate) {
                return candidate.handle == draw.texture &&
                    candidate.atlas_generation_id == frame.atlas_generation_id;
            });
        if (it == cache.textures_.end()) {
            return false;
        }
    }
    return true;
}

} // namespace zevryon::text
