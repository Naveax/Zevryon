#include "gpu_atlas_frame_submission.hpp"

#include <algorithm>
#include <limits>
#include <new>

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

bool add_u64(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t* output) noexcept {
    if (output == nullptr ||
        left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    *output = left + right;
    return true;
}

bool surface_valid(const GpuSurfaceDescriptor& surface) noexcept {
    return surface.surface_id != 0U && surface.generation_id != 0U &&
        surface.width != 0U && surface.height != 0U &&
        surface.reserved == 0U && surface.reserved2 == 0U &&
        surface.premultiplied_alpha <= 1U;
}

GlyphRasterFormat format_for_mode(GlyphRasterMode mode) noexcept {
    switch (mode) {
        case GlyphRasterMode::Grayscale:
            return GlyphRasterFormat::Alpha8;
        case GlyphRasterMode::Lcd:
            return GlyphRasterFormat::LcdRgb8;
        case GlyphRasterMode::Color:
            return GlyphRasterFormat::Bgra8;
    }
    return GlyphRasterFormat::Empty;
}

void clear_submission_error(GpuFrameSubmissionError* error) noexcept {
    if (error != nullptr) {
        error->kind = GpuFrameSubmissionErrorKind::None;
        error->command_index = 0U;
        error->batch_index = 0U;
        error->instance_index = 0U;
        error->page_index = 0U;
        error->message.clear();
    }
}

bool fail_submission(
    GpuFrameSubmissionErrorKind kind,
    std::size_t command_index,
    std::size_t batch_index,
    std::size_t instance_index,
    std::uint32_t page_index,
    const char* message,
    GpuFrameSubmissionError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->command_index = command_index;
        error->batch_index = batch_index;
        error->instance_index = instance_index;
        error->page_index = page_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

void clear_backend_error(GpuFrameBackendError* error) noexcept {
    if (error != nullptr) {
        error->kind = GpuFrameBackendErrorKind::None;
        error->message.clear();
    }
}

bool fail_backend(
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

void clear_submit_error(GpuFrameSubmitError* error) noexcept {
    if (error != nullptr) {
        error->kind = GpuFrameSubmitErrorKind::None;
        error->page_reference_index = 0U;
        error->frame_index = 0U;
        error->page_index = 0U;
        clear_backend_error(&error->backend_error);
        error->message.clear();
    }
}

bool fail_submit(
    GpuFrameSubmitErrorKind kind,
    std::size_t page_reference_index,
    std::size_t frame_index,
    std::uint32_t page_index,
    const char* message,
    const GpuFrameBackendError* backend_error,
    GpuFrameSubmitError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->page_reference_index = page_reference_index;
        error->frame_index = frame_index;
        error->page_index = page_index;
        error->backend_error = backend_error != nullptr
            ? *backend_error
            : GpuFrameBackendError{};
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

void clear_retire_error(GpuFrameRetireError* error) noexcept {
    if (error != nullptr) {
        error->kind = GpuFrameRetireErrorKind::None;
        error->frame_index = 0U;
        error->page_index = 0U;
        error->message.clear();
    }
}

bool fail_retire(
    GpuFrameRetireErrorKind kind,
    std::size_t frame_index,
    std::uint32_t page_index,
    const char* message,
    GpuFrameRetireError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->frame_index = frame_index;
        error->page_index = page_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

std::uint64_t upload_fence_for_page(
    const GlyphAtlasUploadExecution& execution,
    std::uint32_t page_index,
    std::uint64_t page_generation,
    GlyphRasterFormat format,
    bool* found) noexcept {
    std::uint64_t fence = 0U;
    bool matched = false;
    for (std::size_t i = 0; i < execution.batches.size(); ++i) {
        const GlyphAtlasBackendUploadBatch& batch = execution.batches[i];
        const GlyphAtlasUploadReceipt& receipt = execution.receipts[i];
        if (batch.page_index == page_index &&
            batch.page_generation == page_generation &&
            batch.format == format) {
            matched = true;
            fence = std::max(fence, receipt.fence_value);
        }
    }
    if (found != nullptr) {
        *found = matched;
    }
    return fence;
}

bool submission_has_upload_for_page(
    const GlyphAtlasSubmission& submission,
    std::uint32_t page_index,
    std::uint64_t page_generation,
    GlyphRasterFormat format) noexcept {
    return std::any_of(
        submission.uploads.begin(),
        submission.uploads.end(),
        [page_index, page_generation, format](const GlyphAtlasUploadRecord& upload) {
            return upload.page_index == page_index &&
                upload.page_generation == page_generation &&
                upload.format == format;
        });
}

std::size_t find_page_reference(
    const std::pmr::vector<GpuFramePageReference>& pages,
    std::uint32_t page_index) noexcept {
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (pages[i].page_index == page_index) {
            return i;
        }
    }
    return pages.size();
}

bool frame_topology_valid(
    const GpuFrameSubmission& frame,
    std::span<const GlyphAtlasDrawInstance> instances) noexcept {
    if (!surface_valid(frame.surface) || frame.frame_id == 0U ||
        frame.atlas_generation_id == 0U || frame.clips.empty()) {
        return false;
    }
    for (std::size_t page_index = 0;
         page_index < frame.page_references.size();
         ++page_index) {
        const GpuFramePageReference& page = frame.page_references[page_index];
        if (page.page_generation == 0U || page.batch_count == 0U ||
            page.first_batch >= frame.glyph_batches.size() ||
            page.reserved != 0U) {
            return false;
        }
        for (std::size_t previous = 0; previous < page_index; ++previous) {
            if (frame.page_references[previous].page_index == page.page_index) {
                return false;
            }
        }
        std::size_t first = frame.glyph_batches.size();
        std::uint64_t count = 0U;
        for (std::size_t batch_index = 0;
             batch_index < frame.glyph_batches.size();
             ++batch_index) {
            if (frame.glyph_batches[batch_index].page_reference_index ==
                page_index) {
                first = std::min(first, batch_index);
                ++count;
            }
        }
        if (first != page.first_batch || count != page.batch_count) {
            return false;
        }
    }
    for (std::size_t i = 0; i < frame.glyph_batches.size(); ++i) {
        const GpuFrameGlyphBatch& batch = frame.glyph_batches[i];
        if (batch.instance_count == 0U ||
            batch.first_instance > instances.size() ||
            batch.instance_count > instances.size() - batch.first_instance ||
            batch.clip_index >= frame.clips.size() ||
            batch.page_reference_index >= frame.page_references.size()) {
            return false;
        }
        const GpuFramePageReference& page =
            frame.page_references[batch.page_reference_index];
        if (page.page_index != batch.page_index ||
            page.page_generation != batch.page_generation) {
            return false;
        }
        for (std::size_t j = 0; j < batch.instance_count; ++j) {
            const GlyphAtlasDrawInstance& instance =
                instances[batch.first_instance + j];
            if (instance.atlas_generation_id != frame.atlas_generation_id ||
                instance.page_generation != batch.page_generation ||
                instance.page_index != batch.page_index ||
                instance.style_id != batch.style_id ||
                instance.clip_index != batch.clip_index) {
                return false;
            }
        }
    }
    for (const GpuFrameCommandRecord& command : frame.commands) {
        if (command.clip_index >= frame.clips.size()) {
            return false;
        }
        switch (command.kind) {
            case GpuFrameCommandKind::FillRect:
                if (command.payload_index >= frame.fill_rects.size()) {
                    return false;
                }
                break;
            case GpuFrameCommandKind::GlyphBatch:
                if (command.payload_index >= frame.glyph_batches.size()) {
                    return false;
                }
                break;
        }
    }
    return true;
}

std::size_t find_resident_page(
    const std::pmr::vector<GpuAtlasResidentPage>& pages,
    std::uint32_t page_index) noexcept {
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (pages[i].page_index == page_index) {
            return i;
        }
    }
    return pages.size();
}

bool scheduler_config_valid(const GpuAtlasFrameSchedulerConfig& config) noexcept {
    return config.maximum_resident_pages != 0U &&
        config.maximum_frames_in_flight != 0U &&
        config.maximum_page_pins != 0U && config.reserved == 0U;
}

} // namespace

GpuFrameSubmission::GpuFrameSubmission(std::pmr::memory_resource* resource)
    : clips(usable_resource(resource)),
      commands(usable_resource(resource)),
      fill_rects(usable_resource(resource)),
      glyph_batches(usable_resource(resource)),
      page_references(usable_resource(resource)) {}

std::pmr::memory_resource* GpuFrameSubmission::resource() const noexcept {
    return commands.get_allocator().resource();
}

void GpuFrameSubmission::release() noexcept {
    surface = {};
    frame_id = 0U;
    atlas_generation_id = 0U;
    atlas_submission_epoch = 0U;
    required_upload_fence = 0U;
    release_vector(&clips);
    release_vector(&commands);
    release_vector(&fill_rects);
    release_vector(&glyph_batches);
    release_vector(&page_references);
}

const char* gpu_frame_submission_error_kind_name(
    GpuFrameSubmissionErrorKind kind) noexcept {
    switch (kind) {
        case GpuFrameSubmissionErrorKind::None: return "none";
        case GpuFrameSubmissionErrorKind::InvalidInput: return "invalid-input";
        case GpuFrameSubmissionErrorKind::StaleAtlasSubmission: return "stale-atlas-submission";
        case GpuFrameSubmissionErrorKind::PaintTopologyViolation: return "paint-topology-violation";
        case GpuFrameSubmissionErrorKind::DrawTopologyViolation: return "draw-topology-violation";
        case GpuFrameSubmissionErrorKind::PageFormatMismatch: return "page-format-mismatch";
        case GpuFrameSubmissionErrorKind::MissingUploadCompletion: return "missing-upload-completion";
        case GpuFrameSubmissionErrorKind::SubmissionLimitExceeded: return "submission-limit-exceeded";
        case GpuFrameSubmissionErrorKind::OutputBudgetExceeded: return "output-budget-exceeded";
        case GpuFrameSubmissionErrorKind::ArithmeticOverflow: return "arithmetic-overflow";
        case GpuFrameSubmissionErrorKind::AggregateOverflow: return "aggregate-overflow";
    }
    return "unknown";
}

bool prepare_gpu_frame_submission(
    const GpuFrameSubmissionRequest& request,
    GpuFrameSubmission* output,
    GpuFrameSubmissionStats* stats,
    GpuFrameSubmissionError* error) noexcept {
    clear_submission_error(error);
    if (output == nullptr) {
        return fail_submission(
            GpuFrameSubmissionErrorKind::InvalidInput,
            0U, 0U, 0U, 0U,
            "GPU frame output is null",
            error);
    }
    output->release();
    if (stats != nullptr) {
        *stats = {};
    }
    if (request.paint_stream == nullptr || request.working_set == nullptr ||
        request.atlas_submission == nullptr || request.upload_execution == nullptr ||
        request.cache == nullptr || request.frame_id == 0U ||
        !surface_valid(request.surface) ||
        request.limits.maximum_clips == 0U ||
        request.limits.maximum_commands == 0U ||
        request.limits.maximum_fill_rects == 0U ||
        request.limits.maximum_glyph_batches == 0U ||
        request.limits.maximum_page_references == 0U ||
        request.limits.maximum_referenced_instances == 0U) {
        return fail_submission(
            GpuFrameSubmissionErrorKind::InvalidInput,
            0U, 0U, 0U, 0U,
            "invalid GPU frame submission request",
            error);
    }
    if (!glyph_atlas_upload_execution_is_current(
            *request.cache,
            *request.atlas_submission,
            *request.upload_execution)) {
        return fail_submission(
            GpuFrameSubmissionErrorKind::StaleAtlasSubmission,
            0U, 0U, 0U, 0U,
            "glyph atlas upload execution is stale",
            error);
    }
    if (request.paint_stream->clips.empty() ||
        request.paint_stream->clips.size() > request.limits.maximum_clips ||
        request.atlas_submission->draw_batches.size() >
            request.limits.maximum_glyph_batches ||
        request.atlas_submission->draw_instances.size() >
            request.limits.maximum_referenced_instances) {
        return fail_submission(
            GpuFrameSubmissionErrorKind::SubmissionLimitExceeded,
            0U, 0U, 0U, 0U,
            "GPU frame input exceeds caller limits",
            error);
    }

    try {
        std::pmr::vector<TextPaintClipRect> staged_clips(output->resource());
        std::pmr::vector<GpuFrameCommandRecord> staged_commands(output->resource());
        std::pmr::vector<TextPaintFillRect> staged_fills(output->resource());
        std::pmr::vector<GpuFrameGlyphBatch> staged_batches(output->resource());
        std::pmr::vector<GpuFramePageReference> staged_pages(output->resource());

        staged_clips.assign(
            request.paint_stream->clips.begin(),
            request.paint_stream->clips.end());

        enum class PaintPhase : std::uint8_t { Selection = 0, Glyph, Caret };
        PaintPhase phase = PaintPhase::Selection;
        std::uint64_t selection_commands = 0U;
        std::uint64_t caret_commands = 0U;
        std::uint64_t paint_glyph_commands = 0U;

        for (std::size_t i = 0; i < request.paint_stream->commands.size(); ++i) {
            const TextPaintCommandRecord& command = request.paint_stream->commands[i];
            if (command.clip_index >= request.paint_stream->clips.size()) {
                return fail_submission(
                    GpuFrameSubmissionErrorKind::PaintTopologyViolation,
                    i, 0U, 0U, 0U,
                    "paint command clip index is invalid",
                    error);
            }
            switch (command.kind) {
                case TextPaintCommandKind::SelectionRect:
                    if (phase != PaintPhase::Selection ||
                        command.payload_index >= request.paint_stream->fill_rects.size()) {
                        return fail_submission(
                            GpuFrameSubmissionErrorKind::PaintTopologyViolation,
                            i, 0U, 0U, 0U,
                            "selection command violates paint partition",
                            error);
                    }
                    if (staged_fills.size() >= request.limits.maximum_fill_rects ||
                        staged_commands.size() >= request.limits.maximum_commands) {
                        return fail_submission(
                            GpuFrameSubmissionErrorKind::SubmissionLimitExceeded,
                            i, 0U, 0U, 0U,
                            "GPU frame fill or command limit exceeded",
                            error);
                    }
                    staged_fills.push_back(
                        request.paint_stream->fill_rects[command.payload_index]);
                    staged_commands.push_back({
                        GpuFrameCommandKind::FillRect,
                        static_cast<std::uint32_t>(staged_fills.size() - 1U),
                        command.clip_index,
                        command.flags});
                    ++selection_commands;
                    break;
                case TextPaintCommandKind::GlyphBatch:
                    if (phase == PaintPhase::Caret ||
                        command.payload_index >= request.paint_stream->glyph_batches.size()) {
                        return fail_submission(
                            GpuFrameSubmissionErrorKind::PaintTopologyViolation,
                            i, 0U, 0U, 0U,
                            "glyph command violates paint partition",
                            error);
                    }
                    phase = PaintPhase::Glyph;
                    ++paint_glyph_commands;
                    break;
                case TextPaintCommandKind::CaretRect:
                    if (command.payload_index >= request.paint_stream->fill_rects.size()) {
                        return fail_submission(
                            GpuFrameSubmissionErrorKind::PaintTopologyViolation,
                            i, 0U, 0U, 0U,
                            "caret command fill index is invalid",
                            error);
                    }
                    phase = PaintPhase::Caret;
                    ++caret_commands;
                    break;
            }
        }

        if (paint_glyph_commands == 0U &&
            !request.atlas_submission->draw_batches.empty()) {
            return fail_submission(
                GpuFrameSubmissionErrorKind::PaintTopologyViolation,
                0U, 0U, 0U, 0U,
                "atlas draw batches exist without paint glyph commands",
                error);
        }

        std::uint64_t referenced_instances = 0U;
        std::size_t expected_first_instance = 0U;
        for (std::size_t batch_index = 0;
             batch_index < request.atlas_submission->draw_batches.size();
             ++batch_index) {
            const GlyphAtlasDrawBatch& source_batch =
                request.atlas_submission->draw_batches[batch_index];
            if (source_batch.instance_count == 0U ||
                source_batch.first_instance != expected_first_instance ||
                source_batch.first_instance >
                    request.atlas_submission->draw_instances.size() ||
                source_batch.instance_count >
                    request.atlas_submission->draw_instances.size() -
                        source_batch.first_instance ||
                source_batch.clip_index >= staged_clips.size()) {
                return fail_submission(
                    GpuFrameSubmissionErrorKind::DrawTopologyViolation,
                    0U, batch_index, source_batch.first_instance,
                    source_batch.page_index,
                    "atlas draw batches do not partition draw instances",
                    error);
            }

            GlyphRasterFormat batch_format = GlyphRasterFormat::Empty;
            for (std::size_t j = 0; j < source_batch.instance_count; ++j) {
                const std::size_t instance_index = source_batch.first_instance + j;
                const GlyphAtlasDrawInstance& instance =
                    request.atlas_submission->draw_instances[instance_index];
                if (instance.atlas_generation_id !=
                        request.atlas_submission->atlas_generation_id ||
                    instance.page_generation != source_batch.page_generation ||
                    instance.page_index != source_batch.page_index ||
                    instance.style_id != source_batch.style_id ||
                    instance.clip_index != source_batch.clip_index ||
                    instance.working_set_key_index >=
                        request.working_set->entries.size()) {
                    return fail_submission(
                        GpuFrameSubmissionErrorKind::DrawTopologyViolation,
                        0U, batch_index, instance_index,
                        source_batch.page_index,
                        "draw instance does not match its atlas batch",
                        error);
                }
                const GlyphRasterFormat current_format = format_for_mode(
                    request.working_set->entries[
                        instance.working_set_key_index].key.mode);
                if (current_format == GlyphRasterFormat::Empty ||
                    (j != 0U && current_format != batch_format)) {
                    return fail_submission(
                        GpuFrameSubmissionErrorKind::PageFormatMismatch,
                        0U, batch_index, instance_index,
                        source_batch.page_index,
                        "one atlas page contains incompatible raster formats",
                        error);
                }
                batch_format = current_format;
            }

            std::size_t page_reference_index =
                find_page_reference(staged_pages, source_batch.page_index);
            if (page_reference_index == staged_pages.size()) {
                if (staged_pages.size() >=
                    request.limits.maximum_page_references) {
                    return fail_submission(
                        GpuFrameSubmissionErrorKind::SubmissionLimitExceeded,
                        0U, batch_index, source_batch.first_instance,
                        source_batch.page_index,
                        "GPU frame page-reference limit exceeded",
                        error);
                }
                bool completion_found = false;
                const std::uint64_t upload_fence = upload_fence_for_page(
                    *request.upload_execution,
                    source_batch.page_index,
                    source_batch.page_generation,
                    batch_format,
                    &completion_found);
                const bool has_upload = submission_has_upload_for_page(
                    *request.atlas_submission,
                    source_batch.page_index,
                    source_batch.page_generation,
                    batch_format);
                if (has_upload && (!completion_found || upload_fence == 0U)) {
                    return fail_submission(
                        GpuFrameSubmissionErrorKind::MissingUploadCompletion,
                        0U, batch_index, source_batch.first_instance,
                        source_batch.page_index,
                        "draw page has uploads without completed receipt",
                        error);
                }
                GpuFramePageReference page;
                page.page_generation = source_batch.page_generation;
                page.required_upload_fence = upload_fence;
                page.page_index = source_batch.page_index;
                page.first_batch = static_cast<std::uint32_t>(batch_index);
                page.batch_count = 1U;
                page.format = batch_format;
                staged_pages.push_back(page);
                page_reference_index = staged_pages.size() - 1U;
            } else {
                GpuFramePageReference& page = staged_pages[page_reference_index];
                if (page.page_generation != source_batch.page_generation ||
                    page.format != batch_format) {
                    return fail_submission(
                        GpuFrameSubmissionErrorKind::PageFormatMismatch,
                        0U, batch_index, source_batch.first_instance,
                        source_batch.page_index,
                        "one page index resolves to multiple generations or formats",
                        error);
                }
                if (page.batch_count ==
                    std::numeric_limits<std::uint32_t>::max()) {
                    return fail_submission(
                        GpuFrameSubmissionErrorKind::ArithmeticOverflow,
                        0U, batch_index, source_batch.first_instance,
                        source_batch.page_index,
                        "page batch count overflow",
                        error);
                }
                ++page.batch_count;
            }

            if (!add_u64(
                    referenced_instances,
                    source_batch.instance_count,
                    &referenced_instances) ||
                referenced_instances >
                    request.limits.maximum_referenced_instances) {
                return fail_submission(
                    GpuFrameSubmissionErrorKind::SubmissionLimitExceeded,
                    0U, batch_index, source_batch.first_instance,
                    source_batch.page_index,
                    "GPU frame referenced-instance limit exceeded",
                    error);
            }

            GpuFrameGlyphBatch batch;
            batch.page_generation = source_batch.page_generation;
            batch.page_index = source_batch.page_index;
            batch.first_instance = source_batch.first_instance;
            batch.instance_count = source_batch.instance_count;
            batch.style_id = source_batch.style_id;
            batch.clip_index = source_batch.clip_index;
            batch.page_reference_index =
                static_cast<std::uint32_t>(page_reference_index);
            if ((source_batch.flags & kGlyphAtlasDrawBatchCoalesced) != 0U) {
                batch.flags |= kGpuFrameGlyphBatchCoalesced;
            }
            staged_batches.push_back(batch);
            expected_first_instance += source_batch.instance_count;
        }

        if (expected_first_instance !=
            request.atlas_submission->draw_instances.size()) {
            return fail_submission(
                GpuFrameSubmissionErrorKind::DrawTopologyViolation,
                0U, staged_batches.size(), expected_first_instance, 0U,
                "atlas draw batches omit retained draw instances",
                error);
        }

        for (std::size_t batch_index = 0;
             batch_index < staged_batches.size();
             ++batch_index) {
            if (staged_commands.size() >= request.limits.maximum_commands) {
                return fail_submission(
                    GpuFrameSubmissionErrorKind::SubmissionLimitExceeded,
                    0U, batch_index, staged_batches[batch_index].first_instance,
                    staged_batches[batch_index].page_index,
                    "GPU frame command limit exceeded",
                    error);
            }
            staged_commands.push_back({
                GpuFrameCommandKind::GlyphBatch,
                static_cast<std::uint32_t>(batch_index),
                staged_batches[batch_index].clip_index,
                staged_batches[batch_index].flags});
        }

        for (std::size_t i = 0; i < request.paint_stream->commands.size(); ++i) {
            const TextPaintCommandRecord& command = request.paint_stream->commands[i];
            if (command.kind != TextPaintCommandKind::CaretRect) {
                continue;
            }
            if (staged_fills.size() >= request.limits.maximum_fill_rects ||
                staged_commands.size() >= request.limits.maximum_commands) {
                return fail_submission(
                    GpuFrameSubmissionErrorKind::SubmissionLimitExceeded,
                    i, staged_batches.size(), 0U, 0U,
                    "GPU frame caret or command limit exceeded",
                    error);
            }
            staged_fills.push_back(
                request.paint_stream->fill_rects[command.payload_index]);
            staged_commands.push_back({
                GpuFrameCommandKind::FillRect,
                static_cast<std::uint32_t>(staged_fills.size() - 1U),
                command.clip_index,
                command.flags});
        }

        std::uint64_t required_fence = 0U;
        std::uint64_t pages_waiting = 0U;
        std::uint64_t maximum_batches_per_page = 0U;
        for (const GpuFramePageReference& page : staged_pages) {
            required_fence = std::max(required_fence, page.required_upload_fence);
            if (page.required_upload_fence != 0U) {
                ++pages_waiting;
            }
            maximum_batches_per_page =
                std::max<std::uint64_t>(maximum_batches_per_page, page.batch_count);
        }
        if (required_fence > request.upload_execution->last_fence_value) {
            return fail_submission(
                GpuFrameSubmissionErrorKind::MissingUploadCompletion,
                0U, 0U, 0U, 0U,
                "frame requires an upload fence beyond completed execution",
                error);
        }

        output->surface = request.surface;
        output->frame_id = request.frame_id;
        output->atlas_generation_id =
            request.atlas_submission->atlas_generation_id;
        output->atlas_submission_epoch =
            request.atlas_submission->submission_epoch;
        output->required_upload_fence = required_fence;
        output->clips.swap(staged_clips);
        output->commands.swap(staged_commands);
        output->fill_rects.swap(staged_fills);
        output->glyph_batches.swap(staged_batches);
        output->page_references.swap(staged_pages);

        if (stats != nullptr) {
            stats->input_paint_commands = request.paint_stream->commands.size();
            stats->input_draw_instances =
                request.atlas_submission->draw_instances.size();
            stats->input_draw_batches =
                request.atlas_submission->draw_batches.size();
            stats->output_clips = output->clips.size();
            stats->output_commands = output->commands.size();
            stats->output_fill_rects = output->fill_rects.size();
            stats->output_glyph_batches = output->glyph_batches.size();
            stats->output_page_references = output->page_references.size();
            stats->selection_commands = selection_commands;
            stats->caret_commands = caret_commands;
            stats->referenced_instances = referenced_instances;
            stats->coalesced_instances = referenced_instances >= output->glyph_batches.size()
                ? referenced_instances - output->glyph_batches.size()
                : 0U;
            stats->pages_waiting_for_uploads = pages_waiting;
            stats->maximum_batches_per_page = maximum_batches_per_page;
            stats->required_upload_fence = required_fence;
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail_submission(
            GpuFrameSubmissionErrorKind::OutputBudgetExceeded,
            0U, 0U, 0U, 0U,
            "GPU frame output budget exceeded",
            error);
    } catch (...) {
        return fail_submission(
            GpuFrameSubmissionErrorKind::AggregateOverflow,
            0U, 0U, 0U, 0U,
            "unexpected GPU frame preparation failure",
            error);
    }
}

bool gpu_frame_submission_is_current(
    const GlyphAtlasCache& cache,
    const GlyphAtlasSubmission& atlas_submission,
    const GlyphAtlasUploadExecution& upload_execution,
    const GpuFrameSubmission& frame) noexcept {
    if (!glyph_atlas_upload_execution_is_current(
            cache, atlas_submission, upload_execution) ||
        frame.atlas_generation_id != atlas_submission.atlas_generation_id ||
        frame.atlas_submission_epoch != atlas_submission.submission_epoch ||
        frame.required_upload_fence > upload_execution.last_fence_value ||
        !frame_topology_valid(frame, atlas_submission.draw_instances)) {
        return false;
    }
    for (std::size_t i = 0; i < frame.page_references.size(); ++i) {
        const GpuFramePageReference& page = frame.page_references[i];
        for (std::size_t j = i + 1U; j < frame.page_references.size(); ++j) {
            if (frame.page_references[j].page_index == page.page_index) {
                return false;
            }
        }
        bool found = false;
        const std::uint64_t fence = upload_fence_for_page(
            upload_execution,
            page.page_index,
            page.page_generation,
            page.format,
            &found);
        if (page.required_upload_fence != (found ? fence : 0U)) {
            return false;
        }
    }
    return true;
}

GpuAtlasFrameScheduler::GpuAtlasFrameScheduler(
    GpuAtlasFrameSchedulerConfig config,
    std::size_t metadata_hard_limit) noexcept
    : metadata_resource_(ledger_, core::ResourceClass::CompositorSurface),
      pages_(&metadata_resource_),
      frames_(&metadata_resource_),
      pins_(&metadata_resource_),
      config_(config) {
    ledger_.set_hard_limit(
        core::ResourceClass::CompositorSurface,
        metadata_hard_limit);
}

bool GpuAtlasFrameScheduler::clear() noexcept {
    std::scoped_lock lock(mutex_);
    if (!frames_.empty() || !pins_.empty()) {
        return false;
    }
    release_vector(&pages_);
    atlas_generation_id_ = 0U;
    scheduler_generation_ =
        scheduler_generation_ == std::numeric_limits<std::uint64_t>::max()
        ? 1U
        : scheduler_generation_ + 1U;
    return true;
}

GpuAtlasFrameSchedulerSnapshot
GpuAtlasFrameScheduler::snapshot_locked() const noexcept {
    GpuAtlasFrameSchedulerSnapshot snapshot;
    snapshot.metadata = ledger_.snapshot(core::ResourceClass::CompositorSurface);
    snapshot.config = config_;
    snapshot.scheduler_generation = scheduler_generation_;
    snapshot.atlas_generation_id = atlas_generation_id_;
    snapshot.next_ticket_id = next_ticket_id_;
    snapshot.last_submit_fence_value = last_submit_fence_value_;
    snapshot.last_completed_fence_value = last_completed_fence_value_;
    snapshot.submitted_frames = submitted_frames_;
    snapshot.retired_frames = retired_frames_;
    snapshot.page_replacements = page_replacements_;
    snapshot.resident_page_count = pages_.size();
    snapshot.in_flight_frame_count = frames_.size();
    snapshot.page_pin_count = pins_.size();
    return snapshot;
}

GpuAtlasFrameSchedulerSnapshot GpuAtlasFrameScheduler::snapshot() const noexcept {
    std::scoped_lock lock(mutex_);
    return snapshot_locked();
}

GpuFrameBackendKind ReferenceGpuFrameBackend::kind() const noexcept {
    return GpuFrameBackendKind::ReferenceCpu;
}

bool ReferenceGpuFrameBackend::submit(
    const GpuFrameSubmission& frame,
    std::span<const GlyphAtlasDrawInstance> draw_instances,
    std::uint64_t ticket_id,
    std::uint64_t wait_fence_value,
    std::uint64_t* signal_fence_value,
    GpuFrameBackendError* error) noexcept {
    clear_backend_error(error);
    if (ticket_id == 0U || signal_fence_value == nullptr ||
        wait_fence_value != frame.required_upload_fence ||
        !frame_topology_valid(frame, draw_instances)) {
        return fail_backend(
            GpuFrameBackendErrorKind::InvalidInput,
            "invalid reference GPU frame submission",
            error);
    }
    if (wait_fence_value == std::numeric_limits<std::uint64_t>::max()) {
        return fail_backend(
            GpuFrameBackendErrorKind::FenceOverflow,
            "GPU frame wait fence overflow",
            error);
    }
    const std::uint64_t minimum = wait_fence_value + 1U;
    const std::uint64_t fence = std::max(next_fence_value_, minimum);
    if (fence == std::numeric_limits<std::uint64_t>::max()) {
        return fail_backend(
            GpuFrameBackendErrorKind::FenceOverflow,
            "reference GPU frame fence overflow",
            error);
    }
    *signal_fence_value = fence;
    next_fence_value_ = fence + 1U;
    return true;
}

const char* gpu_frame_submit_error_kind_name(
    GpuFrameSubmitErrorKind kind) noexcept {
    switch (kind) {
        case GpuFrameSubmitErrorKind::None: return "none";
        case GpuFrameSubmitErrorKind::InvalidInput: return "invalid-input";
        case GpuFrameSubmitErrorKind::StaleFrame: return "stale-frame";
        case GpuFrameSubmitErrorKind::SchedulerCapacityExceeded: return "scheduler-capacity-exceeded";
        case GpuFrameSubmitErrorKind::PageNotResident: return "page-not-resident";
        case GpuFrameSubmitErrorKind::PagePinned: return "page-pinned";
        case GpuFrameSubmitErrorKind::PageNotReady: return "page-not-ready";
        case GpuFrameSubmitErrorKind::BackendFailure: return "backend-failure";
        case GpuFrameSubmitErrorKind::MetadataBudgetExceeded: return "metadata-budget-exceeded";
        case GpuFrameSubmitErrorKind::ArithmeticOverflow: return "arithmetic-overflow";
        case GpuFrameSubmitErrorKind::AggregateOverflow: return "aggregate-overflow";
    }
    return "unknown";
}

bool submit_gpu_frame(
    const GpuFrameSubmitRequest& request,
    GpuFrameBackend* backend,
    GpuAtlasFrameScheduler* scheduler,
    GpuFrameReceipt* receipt,
    GpuFrameSubmitStats* stats,
    GpuFrameSubmitError* error) noexcept {
    clear_submit_error(error);
    if (receipt != nullptr) {
        *receipt = {};
    }
    if (stats != nullptr) {
        *stats = {};
    }
    if (request.frame == nullptr || request.atlas_submission == nullptr ||
        request.upload_execution == nullptr || request.cache == nullptr ||
        backend == nullptr || scheduler == nullptr || receipt == nullptr ||
        !scheduler_config_valid(scheduler->config_)) {
        return fail_submit(
            GpuFrameSubmitErrorKind::InvalidInput,
            0U, 0U, 0U,
            "invalid GPU frame submit request",
            nullptr,
            error);
    }
    if (!gpu_frame_submission_is_current(
            *request.cache,
            *request.atlas_submission,
            *request.upload_execution,
            *request.frame)) {
        return fail_submit(
            GpuFrameSubmitErrorKind::StaleFrame,
            0U, 0U, 0U,
            "GPU frame is stale before submission",
            nullptr,
            error);
    }

    std::scoped_lock lock(scheduler->mutex_);
    if (stats != nullptr) {
        stats->metadata_before = scheduler->ledger_.snapshot(
            core::ResourceClass::CompositorSurface);
    }
    if (scheduler->frames_.size() >=
            scheduler->config_.maximum_frames_in_flight ||
        request.frame->page_references.size() >
            scheduler->config_.maximum_page_pins -
                std::min<std::size_t>(
                    scheduler->pins_.size(),
                    scheduler->config_.maximum_page_pins)) {
        return fail_submit(
            GpuFrameSubmitErrorKind::SchedulerCapacityExceeded,
            0U, scheduler->frames_.size(), 0U,
            "GPU frame scheduler in-flight capacity exceeded",
            nullptr,
            error);
    }

    try {
        std::pmr::vector<GpuAtlasResidentPage> staged_pages(
            &scheduler->metadata_resource_);
        std::pmr::vector<GpuInFlightFrameRecord> staged_frames(
            &scheduler->metadata_resource_);
        std::pmr::vector<GpuFramePagePin> staged_pins(
            &scheduler->metadata_resource_);
        staged_pages.assign(scheduler->pages_.begin(), scheduler->pages_.end());
        staged_frames.assign(scheduler->frames_.begin(), scheduler->frames_.end());
        staged_pins.assign(scheduler->pins_.begin(), scheduler->pins_.end());

        std::uint64_t staged_atlas_generation = scheduler->atlas_generation_id_;
        std::uint64_t page_replacements = 0U;
        if (staged_atlas_generation == 0U) {
            staged_atlas_generation = request.frame->atlas_generation_id;
        } else if (staged_atlas_generation != request.frame->atlas_generation_id) {
            if (!staged_frames.empty() || !staged_pins.empty()) {
                return fail_submit(
                    GpuFrameSubmitErrorKind::PagePinned,
                    0U, 0U, 0U,
                    "atlas generation cannot change while frames are in flight",
                    nullptr,
                    error);
            }
            staged_pages.clear();
            staged_atlas_generation = request.frame->atlas_generation_id;
        }

        std::uint64_t synchronized_pages = 0U;
        for (std::size_t i = 0; i < request.upload_execution->receipts.size(); ++i) {
            const GlyphAtlasUploadReceipt& upload =
                request.upload_execution->receipts[i];
            const GlyphAtlasBackendUploadBatch& batch =
                request.upload_execution->batches[i];
            std::size_t page_index = find_resident_page(
                staged_pages, upload.page_index);
            if (page_index == staged_pages.size()) {
                if (staged_pages.size() >=
                    scheduler->config_.maximum_resident_pages) {
                    return fail_submit(
                        GpuFrameSubmitErrorKind::SchedulerCapacityExceeded,
                        0U, staged_frames.size(), upload.page_index,
                        "GPU resident page capacity exceeded",
                        nullptr,
                        error);
                }
                GpuAtlasResidentPage page;
                page.atlas_generation_id = upload.atlas_generation_id;
                page.page_generation = upload.page_generation;
                page.ready_fence_value = upload.fence_value;
                page.page_index = upload.page_index;
                page.format = batch.format;
                page.initialized = 1U;
                staged_pages.push_back(page);
                ++synchronized_pages;
                continue;
            }
            GpuAtlasResidentPage& page = staged_pages[page_index];
            if (page.atlas_generation_id != upload.atlas_generation_id ||
                page.page_generation != upload.page_generation ||
                page.format != batch.format) {
                if (page.pin_count != 0U) {
                    return fail_submit(
                        GpuFrameSubmitErrorKind::PagePinned,
                        0U, staged_frames.size(), upload.page_index,
                        "GPU atlas page replacement is blocked by an in-flight frame",
                        nullptr,
                        error);
                }
                page = {};
                page.atlas_generation_id = upload.atlas_generation_id;
                page.page_generation = upload.page_generation;
                page.page_index = upload.page_index;
                page.format = batch.format;
                page.initialized = 1U;
                ++page_replacements;
            }
            page.ready_fence_value =
                std::max(page.ready_fence_value, upload.fence_value);
            ++synchronized_pages;
        }

        const std::size_t first_pin = staged_pins.size();
        std::uint64_t reused_pages = 0U;
        for (std::size_t i = 0; i < request.frame->page_references.size(); ++i) {
            const GpuFramePageReference& reference =
                request.frame->page_references[i];
            const std::size_t resident_index = find_resident_page(
                staged_pages, reference.page_index);
            if (resident_index == staged_pages.size()) {
                return fail_submit(
                    GpuFrameSubmitErrorKind::PageNotResident,
                    i, staged_frames.size(), reference.page_index,
                    "GPU frame references a non-resident atlas page",
                    nullptr,
                    error);
            }
            GpuAtlasResidentPage& page = staged_pages[resident_index];
            if (page.atlas_generation_id != request.frame->atlas_generation_id ||
                page.page_generation != reference.page_generation ||
                page.format != reference.format || page.initialized == 0U) {
                return fail_submit(
                    GpuFrameSubmitErrorKind::PageNotResident,
                    i, staged_frames.size(), reference.page_index,
                    "GPU resident page identity does not match frame reference",
                    nullptr,
                    error);
            }
            if (page.ready_fence_value < reference.required_upload_fence) {
                return fail_submit(
                    GpuFrameSubmitErrorKind::PageNotReady,
                    i, staged_frames.size(), reference.page_index,
                    "GPU atlas page upload fence is incomplete",
                    nullptr,
                    error);
            }
            if (page.pin_count == std::numeric_limits<std::uint32_t>::max() ||
                page.submitted_frames == std::numeric_limits<std::uint32_t>::max()) {
                return fail_submit(
                    GpuFrameSubmitErrorKind::ArithmeticOverflow,
                    i, staged_frames.size(), reference.page_index,
                    "GPU page pin or submission count overflow",
                    nullptr,
                    error);
            }
            if (reference.required_upload_fence == 0U) {
                ++reused_pages;
            }
            ++page.pin_count;
            ++page.submitted_frames;
            page.last_frame_id = request.frame->frame_id;
            GpuFramePagePin pin;
            pin.frame_id = request.frame->frame_id;
            pin.atlas_generation_id = request.frame->atlas_generation_id;
            pin.page_generation = reference.page_generation;
            pin.page_index = reference.page_index;
            pin.resident_page_index = static_cast<std::uint32_t>(resident_index);
            staged_pins.push_back(pin);
        }

        if (scheduler->next_ticket_id_ == 0U ||
            scheduler->next_ticket_id_ ==
                std::numeric_limits<std::uint64_t>::max()) {
            return fail_submit(
                GpuFrameSubmitErrorKind::ArithmeticOverflow,
                0U, staged_frames.size(), 0U,
                "GPU frame ticket overflow",
                nullptr,
                error);
        }

        GpuInFlightFrameRecord frame_record;
        frame_record.frame_id = request.frame->frame_id;
        frame_record.ticket_id = scheduler->next_ticket_id_;
        frame_record.atlas_generation_id = request.frame->atlas_generation_id;
        frame_record.scheduler_generation = scheduler->scheduler_generation_;
        frame_record.first_pin = static_cast<std::uint32_t>(first_pin);
        frame_record.pin_count = static_cast<std::uint32_t>(
            request.frame->page_references.size());
        frame_record.command_count = static_cast<std::uint32_t>(
            request.frame->commands.size());

        std::uint64_t signal_fence = 0U;
        GpuFrameBackendError backend_error;
        if (!backend->submit(
                *request.frame,
                request.atlas_submission->draw_instances,
                frame_record.ticket_id,
                request.frame->required_upload_fence,
                &signal_fence,
                &backend_error)) {
            return fail_submit(
                GpuFrameSubmitErrorKind::BackendFailure,
                0U, staged_frames.size(), 0U,
                "GPU frame backend submission failed",
                &backend_error,
                error);
        }
        if (signal_fence <= scheduler->last_submit_fence_value_ ||
            signal_fence <= request.frame->required_upload_fence) {
            return fail_submit(
                GpuFrameSubmitErrorKind::BackendFailure,
                0U, staged_frames.size(), 0U,
                "GPU frame backend returned a non-monotone fence",
                nullptr,
                error);
        }
        if (!gpu_frame_submission_is_current(
                *request.cache,
                *request.atlas_submission,
                *request.upload_execution,
                *request.frame)) {
            return fail_submit(
                GpuFrameSubmitErrorKind::StaleFrame,
                0U, staged_frames.size(), 0U,
                "GPU frame became stale during backend submission",
                nullptr,
                error);
        }

        frame_record.submit_fence_value = signal_fence;
        staged_frames.push_back(frame_record);
        scheduler->pages_.swap(staged_pages);
        scheduler->frames_.swap(staged_frames);
        scheduler->pins_.swap(staged_pins);
        scheduler->atlas_generation_id_ = staged_atlas_generation;
        scheduler->last_submit_fence_value_ = signal_fence;
        ++scheduler->next_ticket_id_;
        ++scheduler->submitted_frames_;
        scheduler->page_replacements_ += page_replacements;

        receipt->frame_id = request.frame->frame_id;
        receipt->ticket_id = frame_record.ticket_id;
        receipt->submit_fence_value = signal_fence;
        receipt->atlas_generation_id = request.frame->atlas_generation_id;
        receipt->scheduler_generation = scheduler->scheduler_generation_;
        receipt->command_count = frame_record.command_count;
        receipt->page_reference_count = frame_record.pin_count;
        receipt->status = GpuFrameReceiptStatus::Submitted;

        if (stats != nullptr) {
            stats->metadata_after = scheduler->ledger_.snapshot(
                core::ResourceClass::CompositorSurface);
            stats->input_commands = request.frame->commands.size();
            stats->input_page_references =
                request.frame->page_references.size();
            stats->synchronized_upload_pages = synchronized_pages;
            stats->reused_resident_pages = reused_pages;
            stats->pinned_pages = request.frame->page_references.size();
            stats->signal_fence_value = signal_fence;
            stats->frames_in_flight = scheduler->frames_.size();
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail_submit(
            GpuFrameSubmitErrorKind::MetadataBudgetExceeded,
            0U, scheduler->frames_.size(), 0U,
            "GPU frame scheduler metadata budget exceeded",
            nullptr,
            error);
    } catch (...) {
        return fail_submit(
            GpuFrameSubmitErrorKind::AggregateOverflow,
            0U, scheduler->frames_.size(), 0U,
            "unexpected GPU frame submission failure",
            nullptr,
            error);
    }
}

const char* gpu_frame_retire_error_kind_name(
    GpuFrameRetireErrorKind kind) noexcept {
    switch (kind) {
        case GpuFrameRetireErrorKind::None: return "none";
        case GpuFrameRetireErrorKind::InvalidInput: return "invalid-input";
        case GpuFrameRetireErrorKind::FenceRegression: return "fence-regression";
        case GpuFrameRetireErrorKind::PinTopologyViolation: return "pin-topology-violation";
        case GpuFrameRetireErrorKind::AggregateOverflow: return "aggregate-overflow";
    }
    return "unknown";
}

bool retire_gpu_frames(
    GpuAtlasFrameScheduler* scheduler,
    std::uint64_t completed_fence_value,
    GpuFrameRetireStats* stats,
    GpuFrameRetireError* error) noexcept {
    clear_retire_error(error);
    if (stats != nullptr) {
        *stats = {};
    }
    if (scheduler == nullptr || completed_fence_value == 0U) {
        return fail_retire(
            GpuFrameRetireErrorKind::InvalidInput,
            0U, 0U,
            "invalid GPU frame retirement request",
            error);
    }
    std::scoped_lock lock(scheduler->mutex_);
    if (completed_fence_value < scheduler->last_completed_fence_value_) {
        return fail_retire(
            GpuFrameRetireErrorKind::FenceRegression,
            0U, 0U,
            "completed GPU fence regressed",
            error);
    }
    try {
        std::size_t retire_count = 0U;
        std::size_t released_pins = 0U;
        while (retire_count < scheduler->frames_.size() &&
               scheduler->frames_[retire_count].submit_fence_value <=
                   completed_fence_value) {
            const GpuInFlightFrameRecord& frame =
                scheduler->frames_[retire_count];
            if (frame.first_pin != released_pins ||
                frame.first_pin > scheduler->pins_.size() ||
                frame.pin_count > scheduler->pins_.size() - frame.first_pin) {
                return fail_retire(
                    GpuFrameRetireErrorKind::PinTopologyViolation,
                    retire_count, 0U,
                    "in-flight frame pin ranges are not contiguous",
                    error);
            }
            for (std::size_t i = 0; i < frame.pin_count; ++i) {
                const GpuFramePagePin& pin =
                    scheduler->pins_[frame.first_pin + i];
                if (pin.frame_id != frame.frame_id ||
                    pin.resident_page_index >= scheduler->pages_.size()) {
                    return fail_retire(
                        GpuFrameRetireErrorKind::PinTopologyViolation,
                        retire_count, pin.page_index,
                        "GPU frame pin does not match in-flight frame",
                        error);
                }
                const GpuAtlasResidentPage& page =
                    scheduler->pages_[pin.resident_page_index];
                if (page.atlas_generation_id != pin.atlas_generation_id ||
                    page.page_generation != pin.page_generation ||
                    page.page_index != pin.page_index || page.pin_count == 0U) {
                    return fail_retire(
                        GpuFrameRetireErrorKind::PinTopologyViolation,
                        retire_count, pin.page_index,
                        "GPU resident page pin identity is stale",
                        error);
                }
            }
            released_pins += frame.pin_count;
            ++retire_count;
        }

        // Mutate only after the full retiring prefix has passed topology
        // validation, preserving failure atomicity without extra allocation.
        for (std::size_t i = 0; i < released_pins; ++i) {
            const GpuFramePagePin& pin = scheduler->pins_[i];
            --scheduler->pages_[pin.resident_page_index].pin_count;
        }

        if (released_pins != 0U) {
            scheduler->pins_.erase(
                scheduler->pins_.begin(),
                scheduler->pins_.begin() +
                    static_cast<std::ptrdiff_t>(released_pins));
            for (std::size_t i = retire_count;
                 i < scheduler->frames_.size();
                 ++i) {
                scheduler->frames_[i].first_pin -=
                    static_cast<std::uint32_t>(released_pins);
            }
        }
        if (retire_count != 0U) {
            scheduler->frames_.erase(
                scheduler->frames_.begin(),
                scheduler->frames_.begin() +
                    static_cast<std::ptrdiff_t>(retire_count));
            scheduler->retired_frames_ += retire_count;
        }
        scheduler->last_completed_fence_value_ = completed_fence_value;
        if (stats != nullptr) {
            stats->completed_fence_value = completed_fence_value;
            stats->retired_frames = retire_count;
            stats->released_page_pins = released_pins;
            stats->remaining_frames = scheduler->frames_.size();
            stats->remaining_page_pins = scheduler->pins_.size();
        }
        return true;
    } catch (...) {
        return fail_retire(
            GpuFrameRetireErrorKind::AggregateOverflow,
            0U, 0U,
            "unexpected GPU frame retirement failure",
            error);
    }
}

bool gpu_frame_receipt_is_current(
    const GpuAtlasFrameScheduler& scheduler,
    const GpuFrameReceipt& receipt) noexcept {
    std::scoped_lock lock(scheduler.mutex_);
    if (receipt.status != GpuFrameReceiptStatus::Submitted ||
        receipt.frame_id == 0U || receipt.ticket_id == 0U ||
        receipt.submit_fence_value == 0U ||
        receipt.scheduler_generation != scheduler.scheduler_generation_) {
        return false;
    }
    return std::any_of(
        scheduler.frames_.begin(),
        scheduler.frames_.end(),
        [&receipt](const GpuInFlightFrameRecord& frame) {
            return frame.frame_id == receipt.frame_id &&
                frame.ticket_id == receipt.ticket_id &&
                frame.submit_fence_value == receipt.submit_fence_value &&
                frame.atlas_generation_id == receipt.atlas_generation_id &&
                frame.scheduler_generation == receipt.scheduler_generation &&
                frame.command_count == receipt.command_count &&
                frame.pin_count == receipt.page_reference_count;
        });
}

} // namespace zevryon::text
