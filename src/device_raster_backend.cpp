#include "device_raster_backend.hpp"

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

bool add_u64(std::uint64_t left, std::uint64_t right, std::uint64_t* out) noexcept {
    if (out == nullptr || left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    *out = left + right;
    return true;
}

bool mul_u64(std::uint64_t left, std::uint64_t right, std::uint64_t* out) noexcept {
    if (out == nullptr || (right != 0U && left > std::numeric_limits<std::uint64_t>::max() / right)) {
        return false;
    }
    *out = left * right;
    return true;
}

bool scale_16_16(std::int32_t value, std::uint32_t scale, std::int32_t* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    const std::int64_t product = static_cast<std::int64_t>(value) * static_cast<std::int64_t>(scale);
    const std::int64_t bias = product >= 0 ? 32'768LL : -32'768LL;
    const std::int64_t rounded = (product + bias) / 65'536LL;
    if (rounded < std::numeric_limits<std::int32_t>::min() ||
        rounded > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    *out = static_cast<std::int32_t>(rounded);
    return true;
}

std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const std::byte value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

bool raster_key_less(const GlyphRasterKey& left, const GlyphRasterKey& right) noexcept {
    return std::tie(left.font_generation_id, left.face_id, left.glyph_id,
               left.x_scale, left.y_scale, left.mode, left.subpixel_x,
               left.subpixel_y, left.reserved) <
        std::tie(right.font_generation_id, right.face_id, right.glyph_id,
               right.x_scale, right.y_scale, right.mode, right.subpixel_x,
               right.subpixel_y, right.reserved);
}

bool resident_contains(std::span<const GlyphRasterKey> resident, const GlyphRasterKey& key) noexcept {
    const auto it = std::lower_bound(resident.begin(), resident.end(), key, raster_key_less);
    return it != resident.end() && *it == key;
}

const DeviceRasterFaceSource* find_face_source(
    std::span<const DeviceRasterFaceSource> sources,
    const GlyphRasterKey& key,
    std::size_t* index) noexcept {
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].font_generation_id == key.font_generation_id &&
            sources[i].face_id == key.face_id) {
            if (index != nullptr) {
                *index = i;
            }
            return &sources[i];
        }
    }
    return nullptr;
}

std::uint8_t phase_count_for(
    GlyphRasterMode mode,
    const DeviceRasterPolicy& policy) noexcept {
    switch (mode) {
        case GlyphRasterMode::Grayscale:
            return policy.grayscale_phase_count;
        case GlyphRasterMode::Lcd:
            return policy.lcd_phase_count;
        case GlyphRasterMode::Color:
            return 1U;
    }
    return 0U;
}

bool policy_valid(const DeviceRasterPolicy& policy) noexcept {
    return policy.device_scale_x_16_16 != 0U &&
        policy.device_scale_y_16_16 != 0U &&
        policy.maximum_dimension != 0U &&
        policy.maximum_glyph_bytes != 0U &&
        policy.grayscale_phase_count != 0U &&
        policy.lcd_phase_count != 0U;
}

void clear_plan_error(DeviceGlyphRasterPlanError* error) noexcept {
    if (error != nullptr) {
        error->kind = DeviceGlyphRasterPlanErrorKind::None;
        error->key_index = 0U;
        error->face_id = kInvalidFontFaceId;
        error->message.clear();
    }
}

bool fail_plan(
    DeviceGlyphRasterPlanErrorKind kind,
    std::size_t key_index,
    FontFaceId face_id,
    const char* message,
    DeviceGlyphRasterPlanError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->key_index = key_index;
        error->face_id = face_id;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

void clear_backend_error(DeviceGlyphRasterBackendError* error) noexcept {
    if (error != nullptr) {
        error->kind = DeviceGlyphRasterBackendErrorKind::None;
        error->message.clear();
    }
}

bool fail_backend(
    DeviceGlyphRasterBackendErrorKind kind,
    const char* message,
    DeviceGlyphRasterBackendError* error) noexcept {
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

void clear_execution_error(DeviceGlyphRasterExecutionError* error) noexcept {
    if (error != nullptr) {
        error->kind = DeviceGlyphRasterExecutionErrorKind::None;
        error->job_index = 0U;
        clear_backend_error(&error->backend_error);
        error->message.clear();
    }
}

bool fail_execution(
    DeviceGlyphRasterExecutionErrorKind kind,
    std::size_t job_index,
    const char* message,
    const DeviceGlyphRasterBackendError* backend_error,
    DeviceGlyphRasterExecutionError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->job_index = job_index;
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

void clear_upload_backend_error(GlyphAtlasUploadBackendError* error) noexcept {
    if (error != nullptr) {
        error->kind = GlyphAtlasUploadBackendErrorKind::None;
        error->message.clear();
    }
}

bool fail_upload_backend(
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

void clear_upload_error(GlyphAtlasUploadExecutionError* error) noexcept {
    if (error != nullptr) {
        error->kind = GlyphAtlasUploadExecutionErrorKind::None;
        error->upload_index = 0U;
        error->batch_index = 0U;
        clear_upload_backend_error(&error->backend_error);
        error->message.clear();
    }
}

bool fail_upload(
    GlyphAtlasUploadExecutionErrorKind kind,
    std::size_t upload_index,
    std::size_t batch_index,
    const char* message,
    const GlyphAtlasUploadBackendError* backend_error,
    GlyphAtlasUploadExecutionError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->upload_index = upload_index;
        error->batch_index = batch_index;
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

bool same_upload_batch(
    const GlyphAtlasUploadRecord& left,
    const GlyphAtlasUploadRecord& right) noexcept {
    return left.atlas_generation_id == right.atlas_generation_id &&
        left.page_generation == right.page_generation &&
        left.page_index == right.page_index &&
        left.format == right.format;
}

} // namespace

DeviceGlyphRasterPlan::DeviceGlyphRasterPlan(std::pmr::memory_resource* resource)
    : jobs(usable_resource(resource)) {}

std::pmr::memory_resource* DeviceGlyphRasterPlan::resource() const noexcept {
    return jobs.get_allocator().resource();
}

void DeviceGlyphRasterPlan::release() noexcept {
    queue_generation = 0U;
    atlas_generation_id = 0U;
    release_vector(&jobs);
}

const char* device_glyph_raster_plan_error_kind_name(
    DeviceGlyphRasterPlanErrorKind kind) noexcept {
    switch (kind) {
        case DeviceGlyphRasterPlanErrorKind::None: return "none";
        case DeviceGlyphRasterPlanErrorKind::InvalidInput: return "invalid-input";
        case DeviceGlyphRasterPlanErrorKind::MissingFaceSource: return "missing-face-source";
        case DeviceGlyphRasterPlanErrorKind::GenerationMismatch: return "generation-mismatch";
        case DeviceGlyphRasterPlanErrorKind::ScaleOverflow: return "scale-overflow";
        case DeviceGlyphRasterPlanErrorKind::JobLimitExceeded: return "job-limit-exceeded";
        case DeviceGlyphRasterPlanErrorKind::OutputBudgetExceeded: return "output-budget-exceeded";
        case DeviceGlyphRasterPlanErrorKind::AggregateOverflow: return "aggregate-overflow";
    }
    return "unknown";
}

bool build_device_glyph_raster_plan(
    const DeviceGlyphRasterPlanRequest& request,
    DeviceGlyphRasterPlan* output,
    DeviceGlyphRasterPlanStats* stats,
    DeviceGlyphRasterPlanError* error) noexcept {
    clear_plan_error(error);
    if (output == nullptr) {
        return fail_plan(DeviceGlyphRasterPlanErrorKind::InvalidInput, 0U,
            kInvalidFontFaceId, "device glyph raster plan output is null", error);
    }
    output->release();
    if (stats != nullptr) {
        *stats = {};
    }
    if (request.working_set == nullptr ||
        request.queue_generation == 0U || request.atlas_generation_id == 0U ||
        !policy_valid(request.policy) || request.limits.maximum_jobs == 0U) {
        return fail_plan(DeviceGlyphRasterPlanErrorKind::InvalidInput, 0U,
            kInvalidFontFaceId, "invalid device glyph raster plan request", error);
    }
    if (!std::is_sorted(request.resident_keys.begin(), request.resident_keys.end(), raster_key_less) ||
        std::adjacent_find(request.resident_keys.begin(), request.resident_keys.end()) != request.resident_keys.end()) {
        return fail_plan(DeviceGlyphRasterPlanErrorKind::InvalidInput, 0U,
            kInvalidFontFaceId, "resident raster key snapshot is not sorted and unique", error);
    }
    try {
        std::pmr::vector<DeviceGlyphRasterJob> staged(output->resource());
        staged.reserve(std::min<std::size_t>(
            request.working_set->entries.size(), request.limits.maximum_jobs));
        std::uint64_t next_job_id = 1U;
        std::uint64_t grayscale_jobs = 0U;
        std::uint64_t lcd_jobs = 0U;
        std::uint64_t color_jobs = 0U;
        std::uint64_t cache_hits = 0U;
        std::uint64_t face_mask_count = 0U;
        std::pmr::vector<std::uint32_t> seen_faces(output->resource());
        seen_faces.reserve(request.face_sources.size());

        for (std::size_t key_index = 0; key_index < request.working_set->entries.size(); ++key_index) {
            const GlyphRasterKey& key = request.working_set->entries[key_index].key;
            if (key.font_generation_id == 0U || key.face_id == kInvalidFontFaceId ||
                key.x_scale == 0 || key.y_scale == 0 ||
                key.reserved != static_cast<std::uint8_t>(request.policy.policy_id & 0xFFU)) {
                return fail_plan(DeviceGlyphRasterPlanErrorKind::InvalidInput,
                    key_index, key.face_id, "invalid or policy-mismatched raster key", error);
            }
            if (resident_contains(request.resident_keys, key)) {
                ++cache_hits;
                continue;
            }
            if (staged.size() >= request.limits.maximum_jobs) {
                return fail_plan(DeviceGlyphRasterPlanErrorKind::JobLimitExceeded,
                    key_index, key.face_id, "device raster job limit exceeded", error);
            }
            std::size_t face_index = 0U;
            const DeviceRasterFaceSource* face =
                find_face_source(request.face_sources, key, &face_index);
            if (face == nullptr || face->resource_id == 0U || face->bytes.empty()) {
                return fail_plan(DeviceGlyphRasterPlanErrorKind::MissingFaceSource,
                    key_index, key.face_id, "missing verified face source for cold glyph", error);
            }
            std::int32_t device_x_scale = 0;
            std::int32_t device_y_scale = 0;
            if (!scale_16_16(key.x_scale, request.policy.device_scale_x_16_16,
                    &device_x_scale) ||
                !scale_16_16(key.y_scale, request.policy.device_scale_y_16_16,
                    &device_y_scale) || device_x_scale == 0 || device_y_scale == 0) {
                return fail_plan(DeviceGlyphRasterPlanErrorKind::ScaleOverflow,
                    key_index, key.face_id, "device raster scale is not representable", error);
            }
            const std::uint8_t phase_count = phase_count_for(key.mode, request.policy);
            if (phase_count == 0U || key.subpixel_x >= phase_count ||
                key.subpixel_y >= phase_count) {
                return fail_plan(DeviceGlyphRasterPlanErrorKind::InvalidInput,
                    key_index, key.face_id, "subpixel phase exceeds policy grid", error);
            }
            std::uint32_t flags = 0U;
            switch (key.mode) {
                case GlyphRasterMode::Grayscale:
                    flags = kDeviceGlyphRasterJobGrayscale;
                    ++grayscale_jobs;
                    break;
                case GlyphRasterMode::Lcd:
                    flags = kDeviceGlyphRasterJobLcd;
                    ++lcd_jobs;
                    break;
                case GlyphRasterMode::Color:
                    if (request.policy.color_policy == DeviceColorGlyphPolicy::Reject) {
                        return fail_plan(DeviceGlyphRasterPlanErrorKind::InvalidInput,
                            key_index, key.face_id, "color glyph rejected by policy", error);
                    }
                    flags = kDeviceGlyphRasterJobColor;
                    ++color_jobs;
                    break;
            }
            DeviceGlyphRasterJob job;
            job.key = key;
            job.queue_generation = request.queue_generation;
            job.job_id = next_job_id++;
            job.face_resource_id = face->resource_id;
            job.device_x_scale = device_x_scale;
            job.device_y_scale = device_y_scale;
            job.working_set_key_index = static_cast<std::uint32_t>(key_index);
            job.face_source_index = static_cast<std::uint32_t>(face_index);
            job.flags = flags;
            staged.push_back(job);
            if (std::find(seen_faces.begin(), seen_faces.end(), job.face_source_index) ==
                seen_faces.end()) {
                seen_faces.push_back(job.face_source_index);
                ++face_mask_count;
            }
        }

        output->queue_generation = request.queue_generation;
        output->atlas_generation_id = request.atlas_generation_id;
        output->jobs.swap(staged);
        if (stats != nullptr) {
            stats->input_unique_keys = request.working_set->entries.size();
            stats->cache_hits = cache_hits;
            stats->cold_jobs = output->jobs.size();
            stats->grayscale_jobs = grayscale_jobs;
            stats->lcd_jobs = lcd_jobs;
            stats->color_jobs = color_jobs;
            stats->referenced_face_sources = face_mask_count;
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail_plan(DeviceGlyphRasterPlanErrorKind::OutputBudgetExceeded,
            0U, kInvalidFontFaceId, "device raster plan output budget exceeded", error);
    } catch (...) {
        return fail_plan(DeviceGlyphRasterPlanErrorKind::AggregateOverflow,
            0U, kInvalidFontFaceId, "unexpected device raster plan failure", error);
    }
}

DeviceRasterBackendKind ReferenceDeviceGlyphRasterBackend::kind() const noexcept {
    return DeviceRasterBackendKind::ReferenceCpu;
}

bool ReferenceDeviceGlyphRasterBackend::query(
    const DeviceGlyphRasterJob& job,
    const DeviceRasterFaceSource& face,
    const DeviceRasterPolicy& policy,
    DeviceGlyphRasterMetrics* metrics,
    DeviceGlyphRasterBackendError* error) noexcept {
    clear_backend_error(error);
    if (metrics == nullptr || !policy_valid(policy) || face.resource_id == 0U ||
        face.bytes.empty() || face.font_generation_id != job.key.font_generation_id ||
        face.face_id != job.key.face_id || face.resource_id != job.face_resource_id) {
        return fail_backend(DeviceGlyphRasterBackendErrorKind::InvalidInput,
            "invalid reference raster query", error);
    }
    *metrics = {};
    if (job.key.mode == GlyphRasterMode::Color &&
        policy.color_policy == DeviceColorGlyphPolicy::Reject) {
        return fail_backend(DeviceGlyphRasterBackendErrorKind::UnsupportedMode,
            "reference backend color mode rejected by policy", error);
    }
    if ((job.key.glyph_id % 29U) == 0U) {
        metrics->format = GlyphRasterFormat::Empty;
        metrics->flags = kDeviceGlyphRasterMetricsEmpty;
        return true;
    }
    const std::uint64_t em_x = static_cast<std::uint64_t>(
        job.device_x_scale < 0 ? -static_cast<std::int64_t>(job.device_x_scale) : job.device_x_scale);
    const std::uint64_t em_y = static_cast<std::uint64_t>(
        job.device_y_scale < 0 ? -static_cast<std::int64_t>(job.device_y_scale) : job.device_y_scale);
    const std::uint32_t base_w = static_cast<std::uint32_t>(std::max<std::uint64_t>(1U, em_x / 64U));
    const std::uint32_t base_h = static_cast<std::uint32_t>(std::max<std::uint64_t>(1U, em_y / 64U));
    metrics->width = std::min<std::uint32_t>(
        policy.maximum_dimension, 1U + (job.key.glyph_id % std::max<std::uint32_t>(1U, base_w)));
    metrics->height = std::min<std::uint32_t>(
        policy.maximum_dimension, 1U + ((job.key.glyph_id * 7U + 3U) % std::max<std::uint32_t>(1U, base_h)));
    std::uint32_t bytes_per_pixel = 1U;
    switch (job.key.mode) {
        case GlyphRasterMode::Grayscale:
            metrics->format = GlyphRasterFormat::Alpha8;
            bytes_per_pixel = 1U;
            break;
        case GlyphRasterMode::Lcd:
            metrics->format = GlyphRasterFormat::LcdRgb8;
            bytes_per_pixel = 3U;
            break;
        case GlyphRasterMode::Color:
            metrics->format = GlyphRasterFormat::Bgra8;
            metrics->flags = kDeviceGlyphRasterMetricsColor;
            bytes_per_pixel = 4U;
            break;
    }
    std::uint64_t row_bytes = 0U;
    std::uint64_t payload_size = 0U;
    if (!mul_u64(metrics->width, bytes_per_pixel, &row_bytes) ||
        !mul_u64(row_bytes, metrics->height, &payload_size) ||
        row_bytes > std::numeric_limits<std::uint32_t>::max() ||
        payload_size > policy.maximum_glyph_bytes) {
        return fail_backend(DeviceGlyphRasterBackendErrorKind::ArithmeticOverflow,
            "reference raster metrics exceed policy", error);
    }
    metrics->row_bytes = static_cast<std::uint32_t>(row_bytes);
    metrics->payload_size = payload_size;
    metrics->bearing_x = static_cast<std::int32_t>(job.key.glyph_id % 5U) - 2;
    metrics->bearing_y = static_cast<std::int32_t>(metrics->height) -
        static_cast<std::int32_t>(job.key.glyph_id % 3U);
    return true;
}

bool ReferenceDeviceGlyphRasterBackend::render(
    const DeviceGlyphRasterJob& job,
    const DeviceRasterFaceSource& face,
    const DeviceRasterPolicy& policy,
    const DeviceGlyphRasterMetrics& metrics,
    std::span<std::byte> destination,
    DeviceGlyphRasterBackendError* error) noexcept {
    clear_backend_error(error);
    if (!policy_valid(policy) || face.resource_id != job.face_resource_id ||
        destination.size() != metrics.payload_size) {
        return fail_backend(DeviceGlyphRasterBackendErrorKind::OutputTooSmall,
            "reference raster destination size mismatch", error);
    }
    if ((metrics.flags & kDeviceGlyphRasterMetricsEmpty) != 0U) {
        if (!destination.empty()) {
            return fail_backend(DeviceGlyphRasterBackendErrorKind::RenderFailed,
                "empty glyph received non-empty destination", error);
        }
        return true;
    }
    std::uint64_t state = face.resource_id ^ job.key.font_generation_id ^
        (static_cast<std::uint64_t>(job.key.glyph_id) << 17U) ^
        (static_cast<std::uint64_t>(job.key.subpixel_x) << 9U) ^
        (static_cast<std::uint64_t>(job.key.subpixel_y) << 1U) ^
        static_cast<std::uint64_t>(job.key.mode);
    for (std::size_t i = 0; i < destination.size(); ++i) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        std::uint8_t value = static_cast<std::uint8_t>((state + i * 31U) & 0xFFU);
        if (metrics.format == GlyphRasterFormat::Bgra8 && (i % 4U) == 3U) {
            value = 0xFFU;
        }
        destination[i] = static_cast<std::byte>(value);
    }
    return true;
}

DeviceGlyphRasterSourceSet::DeviceGlyphRasterSourceSet(
    std::pmr::memory_resource* resource)
    : sources(usable_resource(resource)), payload(usable_resource(resource)) {}

std::pmr::memory_resource* DeviceGlyphRasterSourceSet::resource() const noexcept {
    return sources.get_allocator().resource();
}

void DeviceGlyphRasterSourceSet::release() noexcept {
    queue_generation = 0U;
    release_vector(&sources);
    release_vector(&payload);
}

const char* device_glyph_raster_execution_error_kind_name(
    DeviceGlyphRasterExecutionErrorKind kind) noexcept {
    switch (kind) {
        case DeviceGlyphRasterExecutionErrorKind::None: return "none";
        case DeviceGlyphRasterExecutionErrorKind::InvalidInput: return "invalid-input";
        case DeviceGlyphRasterExecutionErrorKind::StaleQueueGeneration: return "stale-queue-generation";
        case DeviceGlyphRasterExecutionErrorKind::MissingFaceSource: return "missing-face-source";
        case DeviceGlyphRasterExecutionErrorKind::BackendQueryFailed: return "backend-query-failed";
        case DeviceGlyphRasterExecutionErrorKind::BackendRenderFailed: return "backend-render-failed";
        case DeviceGlyphRasterExecutionErrorKind::InvalidMetrics: return "invalid-metrics";
        case DeviceGlyphRasterExecutionErrorKind::PayloadLimitExceeded: return "payload-limit-exceeded";
        case DeviceGlyphRasterExecutionErrorKind::SourceLimitExceeded: return "source-limit-exceeded";
        case DeviceGlyphRasterExecutionErrorKind::OutputBudgetExceeded: return "output-budget-exceeded";
        case DeviceGlyphRasterExecutionErrorKind::ArithmeticOverflow: return "arithmetic-overflow";
        case DeviceGlyphRasterExecutionErrorKind::AggregateOverflow: return "aggregate-overflow";
    }
    return "unknown";
}

bool execute_device_glyph_raster_plan(
    const DeviceGlyphRasterExecutionRequest& request,
    DeviceGlyphRasterBackend* backend,
    DeviceGlyphRasterSourceSet* output,
    DeviceGlyphRasterExecutionStats* stats,
    DeviceGlyphRasterExecutionError* error) noexcept {
    clear_execution_error(error);
    if (output == nullptr) {
        return fail_execution(DeviceGlyphRasterExecutionErrorKind::InvalidInput,
            0U, "device raster source output is null", nullptr, error);
    }
    output->release();
    if (stats != nullptr) {
        *stats = {};
    }
    if (request.plan == nullptr || backend == nullptr ||
        request.expected_queue_generation == 0U ||
        request.limits.maximum_sources == 0U ||
        request.limits.maximum_payload_bytes == 0U || !policy_valid(request.policy)) {
        return fail_execution(DeviceGlyphRasterExecutionErrorKind::InvalidInput,
            0U, "invalid device raster execution request", nullptr, error);
    }
    if (request.plan->queue_generation != request.expected_queue_generation) {
        return fail_execution(DeviceGlyphRasterExecutionErrorKind::StaleQueueGeneration,
            0U, "device raster plan queue generation is stale", nullptr, error);
    }
    if (request.plan->jobs.size() > request.limits.maximum_sources) {
        return fail_execution(DeviceGlyphRasterExecutionErrorKind::SourceLimitExceeded,
            0U, "device raster source limit exceeded", nullptr, error);
    }
    try {
        std::pmr::vector<DeviceGlyphRasterMetrics> metrics(output->resource());
        metrics.reserve(request.plan->jobs.size());
        std::uint64_t total_payload = 0U;
        std::uint64_t maximum_source = 0U;
        std::uint64_t empty_glyphs = 0U;
        std::uint64_t grayscale = 0U;
        std::uint64_t lcd = 0U;
        std::uint64_t color = 0U;
        for (std::size_t i = 0; i < request.plan->jobs.size(); ++i) {
            const DeviceGlyphRasterJob& job = request.plan->jobs[i];
            if (job.queue_generation != request.expected_queue_generation ||
                job.face_source_index >= request.face_sources.size()) {
                return fail_execution(DeviceGlyphRasterExecutionErrorKind::MissingFaceSource,
                    i, "device raster job references unavailable face source", nullptr, error);
            }
            const DeviceRasterFaceSource& face = request.face_sources[job.face_source_index];
            DeviceGlyphRasterMetrics current;
            DeviceGlyphRasterBackendError backend_error;
            if (!backend->query(job, face, request.policy, &current, &backend_error)) {
                return fail_execution(DeviceGlyphRasterExecutionErrorKind::BackendQueryFailed,
                    i, "device raster backend query failed", &backend_error, error);
            }
            if ((current.flags & kDeviceGlyphRasterMetricsEmpty) != 0U) {
                if (current.payload_size != 0U || current.width != 0U || current.height != 0U ||
                    current.row_bytes != 0U || current.format != GlyphRasterFormat::Empty) {
                    return fail_execution(DeviceGlyphRasterExecutionErrorKind::InvalidMetrics,
                        i, "empty glyph metrics are inconsistent", nullptr, error);
                }
                ++empty_glyphs;
            } else {
                std::uint64_t expected = 0U;
                if (current.width == 0U || current.height == 0U || current.row_bytes == 0U ||
                    !mul_u64(current.row_bytes, current.height, &expected) ||
                    expected != current.payload_size ||
                    current.width > request.policy.maximum_dimension ||
                    current.height > request.policy.maximum_dimension ||
                    current.payload_size > request.policy.maximum_glyph_bytes) {
                    return fail_execution(DeviceGlyphRasterExecutionErrorKind::InvalidMetrics,
                        i, "device raster metrics violate policy", nullptr, error);
                }
            }
            if (!add_u64(total_payload, current.payload_size, &total_payload) ||
                total_payload > request.limits.maximum_payload_bytes) {
                return fail_execution(DeviceGlyphRasterExecutionErrorKind::PayloadLimitExceeded,
                    i, "device raster payload limit exceeded", nullptr, error);
            }
            maximum_source = std::max(maximum_source, current.payload_size);
            switch (current.format) {
                case GlyphRasterFormat::Alpha8: ++grayscale; break;
                case GlyphRasterFormat::LcdRgb8: ++lcd; break;
                case GlyphRasterFormat::Bgra8: ++color; break;
                case GlyphRasterFormat::Empty: break;
            }
            metrics.push_back(current);
        }

        std::pmr::vector<GlyphRasterSourceRecord> staged_sources(output->resource());
        std::pmr::vector<std::byte> staged_payload(output->resource());
        staged_sources.resize(request.plan->jobs.size());
        staged_payload.resize(static_cast<std::size_t>(total_payload));
        std::uint64_t offset = 0U;
        for (std::size_t i = 0; i < request.plan->jobs.size(); ++i) {
            const DeviceGlyphRasterJob& job = request.plan->jobs[i];
            const DeviceGlyphRasterMetrics& current = metrics[i];
            std::span<std::byte> destination;
            if (current.payload_size != 0U) {
                destination = std::span<std::byte>(staged_payload.data(), staged_payload.size()).subspan(
                    static_cast<std::size_t>(offset),
                    static_cast<std::size_t>(current.payload_size));
            }
            DeviceGlyphRasterBackendError backend_error;
            if (!backend->render(job, request.face_sources[job.face_source_index],
                    request.policy, current, destination, &backend_error)) {
                return fail_execution(DeviceGlyphRasterExecutionErrorKind::BackendRenderFailed,
                    i, "device raster backend render failed", &backend_error, error);
            }
            GlyphRasterSourceRecord source;
            source.key = job.key;
            source.payload_offset = offset;
            source.payload_size = current.payload_size;
            source.width = current.width;
            source.height = current.height;
            source.row_bytes = current.row_bytes;
            source.bearing_x = current.bearing_x;
            source.bearing_y = current.bearing_y;
            source.format = current.format;
            source.flags = (current.flags & kDeviceGlyphRasterMetricsEmpty) != 0U ? 1U : 0U;
            source.content_checksum = fnv1a64(destination);
            staged_sources[i] = source;
            offset += current.payload_size;
        }
        output->queue_generation = request.expected_queue_generation;
        output->sources.swap(staged_sources);
        output->payload.swap(staged_payload);
        if (stats != nullptr) {
            stats->input_jobs = request.plan->jobs.size();
            stats->output_sources = output->sources.size();
            stats->output_payload_bytes = output->payload.size();
            stats->empty_glyphs = empty_glyphs;
            stats->grayscale_sources = grayscale;
            stats->lcd_sources = lcd;
            stats->color_sources = color;
            stats->maximum_source_bytes = maximum_source;
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail_execution(DeviceGlyphRasterExecutionErrorKind::OutputBudgetExceeded,
            0U, "device raster output budget exceeded", nullptr, error);
    } catch (...) {
        return fail_execution(DeviceGlyphRasterExecutionErrorKind::AggregateOverflow,
            0U, "unexpected device raster execution failure", nullptr, error);
    }
}

GlyphAtlasUploadExecution::GlyphAtlasUploadExecution(
    std::pmr::memory_resource* resource)
    : batches(usable_resource(resource)), receipts(usable_resource(resource)) {}

std::pmr::memory_resource* GlyphAtlasUploadExecution::resource() const noexcept {
    return batches.get_allocator().resource();
}

void GlyphAtlasUploadExecution::release() noexcept {
    atlas_generation_id = 0U;
    submission_epoch = 0U;
    last_fence_value = 0U;
    release_vector(&batches);
    release_vector(&receipts);
}

bool ReferenceGlyphAtlasUploadBackend::submit(
    const GlyphAtlasBackendUploadBatch& batch,
    std::span<const GlyphAtlasUploadRecord> uploads,
    std::span<const std::byte> payload,
    std::uint64_t ticket_id,
    std::uint64_t* fence_value,
    GlyphAtlasUploadBackendError* error) noexcept {
    clear_upload_backend_error(error);
    if (fence_value == nullptr || ticket_id == 0U || uploads.empty() ||
        batch.upload_count != uploads.size()) {
        return fail_upload_backend(GlyphAtlasUploadBackendErrorKind::InvalidInput,
            "invalid reference upload submission", error);
    }
    for (const GlyphAtlasUploadRecord& upload : uploads) {
        if (upload.atlas_generation_id != batch.atlas_generation_id ||
            upload.page_generation != batch.page_generation ||
            upload.page_index != batch.page_index || upload.format != batch.format ||
            upload.payload_offset > payload.size() ||
            upload.payload_size > payload.size() - upload.payload_offset) {
            return fail_upload_backend(GlyphAtlasUploadBackendErrorKind::InvalidPayload,
                "upload record does not match batch or payload", error);
        }
    }
    if (next_fence_value_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail_upload_backend(GlyphAtlasUploadBackendErrorKind::FenceOverflow,
            "reference upload fence overflow", error);
    }
    *fence_value = next_fence_value_++;
    return true;
}

const char* glyph_atlas_upload_execution_error_kind_name(
    GlyphAtlasUploadExecutionErrorKind kind) noexcept {
    switch (kind) {
        case GlyphAtlasUploadExecutionErrorKind::None: return "none";
        case GlyphAtlasUploadExecutionErrorKind::InvalidInput: return "invalid-input";
        case GlyphAtlasUploadExecutionErrorKind::StaleSubmission: return "stale-submission";
        case GlyphAtlasUploadExecutionErrorKind::InvalidUploadTopology: return "invalid-upload-topology";
        case GlyphAtlasUploadExecutionErrorKind::UploadLimitExceeded: return "upload-limit-exceeded";
        case GlyphAtlasUploadExecutionErrorKind::BackendFailure: return "backend-failure";
        case GlyphAtlasUploadExecutionErrorKind::OutputBudgetExceeded: return "output-budget-exceeded";
        case GlyphAtlasUploadExecutionErrorKind::ArithmeticOverflow: return "arithmetic-overflow";
        case GlyphAtlasUploadExecutionErrorKind::AggregateOverflow: return "aggregate-overflow";
    }
    return "unknown";
}

bool execute_glyph_atlas_uploads(
    const GlyphAtlasUploadExecutionRequest& request,
    GlyphAtlasUploadBackend* backend,
    GlyphAtlasUploadExecution* output,
    GlyphAtlasUploadExecutionStats* stats,
    GlyphAtlasUploadExecutionError* error) noexcept {
    clear_upload_error(error);
    if (output == nullptr) {
        return fail_upload(GlyphAtlasUploadExecutionErrorKind::InvalidInput,
            0U, 0U, "glyph atlas upload output is null", nullptr, error);
    }
    output->release();
    if (stats != nullptr) {
        *stats = {};
    }
    if (request.submission == nullptr || request.cache == nullptr || backend == nullptr ||
        request.limits.maximum_batches == 0U ||
        request.limits.maximum_upload_bytes == 0U) {
        return fail_upload(GlyphAtlasUploadExecutionErrorKind::InvalidInput,
            0U, 0U, "invalid glyph atlas upload execution request", nullptr, error);
    }
    if (!glyph_atlas_submission_is_current(*request.cache, *request.submission)) {
        return fail_upload(GlyphAtlasUploadExecutionErrorKind::StaleSubmission,
            0U, 0U, "glyph atlas submission is stale", nullptr, error);
    }
    try {
        std::pmr::vector<GlyphAtlasBackendUploadBatch> staged_batches(output->resource());
        std::pmr::vector<GlyphAtlasUploadReceipt> staged_receipts(output->resource());
        std::uint64_t upload_bytes = 0U;
        for (std::size_t i = 0; i < request.submission->uploads.size(); ++i) {
            const GlyphAtlasUploadRecord& upload = request.submission->uploads[i];
            if (upload.payload_offset > request.raster_payload.size() ||
                upload.payload_size > request.raster_payload.size() - upload.payload_offset ||
                !add_u64(upload_bytes, upload.payload_size, &upload_bytes) ||
                upload_bytes > request.limits.maximum_upload_bytes) {
                return fail_upload(GlyphAtlasUploadExecutionErrorKind::UploadLimitExceeded,
                    i, staged_batches.size(), "glyph atlas upload payload exceeds limit", nullptr, error);
            }
            if (staged_batches.empty() ||
                !same_upload_batch(request.submission->uploads[staged_batches.back().first_upload], upload)) {
                if (staged_batches.size() >= request.limits.maximum_batches) {
                    return fail_upload(GlyphAtlasUploadExecutionErrorKind::UploadLimitExceeded,
                        i, staged_batches.size(), "glyph atlas backend batch limit exceeded", nullptr, error);
                }
                GlyphAtlasBackendUploadBatch batch;
                batch.atlas_generation_id = upload.atlas_generation_id;
                batch.page_generation = upload.page_generation;
                batch.page_index = upload.page_index;
                batch.first_upload = static_cast<std::uint32_t>(i);
                batch.upload_count = 1U;
                batch.format = upload.format;
                staged_batches.push_back(batch);
            } else {
                ++staged_batches.back().upload_count;
            }
        }
        staged_receipts.resize(staged_batches.size());
        std::uint64_t last_fence = 0U;
        std::uint64_t maximum_per_batch = 0U;
        for (std::size_t batch_index = 0; batch_index < staged_batches.size(); ++batch_index) {
            const GlyphAtlasBackendUploadBatch& batch = staged_batches[batch_index];
            const std::span<const GlyphAtlasUploadRecord> uploads =
                std::span<const GlyphAtlasUploadRecord>(request.submission->uploads.data(), request.submission->uploads.size()).subspan(
                    batch.first_upload, batch.upload_count);
            const std::uint64_t ticket_id = static_cast<std::uint64_t>(batch_index) + 1U;
            std::uint64_t fence = 0U;
            GlyphAtlasUploadBackendError backend_error;
            if (!backend->submit(batch, uploads, request.raster_payload,
                    ticket_id, &fence, &backend_error)) {
                return fail_upload(GlyphAtlasUploadExecutionErrorKind::BackendFailure,
                    batch.first_upload, batch_index, "glyph atlas upload backend failed",
                    &backend_error, error);
            }
            if (fence == 0U || fence <= last_fence) {
                return fail_upload(GlyphAtlasUploadExecutionErrorKind::InvalidUploadTopology,
                    batch.first_upload, batch_index, "upload fences are not strictly increasing",
                    nullptr, error);
            }
            if (!glyph_atlas_submission_is_current(*request.cache, *request.submission)) {
                return fail_upload(GlyphAtlasUploadExecutionErrorKind::StaleSubmission,
                    batch.first_upload, batch_index, "submission became stale during upload",
                    nullptr, error);
            }
            GlyphAtlasUploadReceipt receipt;
            receipt.ticket_id = ticket_id;
            receipt.fence_value = fence;
            receipt.atlas_generation_id = batch.atlas_generation_id;
            receipt.page_generation = batch.page_generation;
            receipt.page_index = batch.page_index;
            receipt.first_upload = batch.first_upload;
            receipt.upload_count = batch.upload_count;
            receipt.status = GlyphAtlasUploadReceiptStatus::Completed;
            staged_receipts[batch_index] = receipt;
            last_fence = fence;
            maximum_per_batch = std::max<std::uint64_t>(maximum_per_batch, batch.upload_count);
        }
        output->atlas_generation_id = request.submission->atlas_generation_id;
        output->submission_epoch = request.submission->submission_epoch;
        output->last_fence_value = last_fence;
        output->batches.swap(staged_batches);
        output->receipts.swap(staged_receipts);
        if (stats != nullptr) {
            stats->input_uploads = request.submission->uploads.size();
            stats->upload_bytes = upload_bytes;
            stats->output_batches = output->batches.size();
            stats->output_receipts = output->receipts.size();
            stats->coalesced_uploads = request.submission->uploads.size() - output->batches.size();
            stats->maximum_uploads_per_batch = maximum_per_batch;
            stats->last_fence_value = last_fence;
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail_upload(GlyphAtlasUploadExecutionErrorKind::OutputBudgetExceeded,
            0U, 0U, "glyph atlas upload output budget exceeded", nullptr, error);
    } catch (...) {
        return fail_upload(GlyphAtlasUploadExecutionErrorKind::AggregateOverflow,
            0U, 0U, "unexpected glyph atlas upload execution failure", nullptr, error);
    }
}

bool glyph_atlas_upload_execution_is_current(
    const GlyphAtlasCache& cache,
    const GlyphAtlasSubmission& submission,
    const GlyphAtlasUploadExecution& execution) noexcept {
    if (!glyph_atlas_submission_is_current(cache, submission) ||
        execution.atlas_generation_id == 0U ||
        execution.atlas_generation_id != submission.atlas_generation_id ||
        execution.submission_epoch != submission.submission_epoch ||
        execution.batches.size() != execution.receipts.size()) {
        return false;
    }
    std::uint64_t previous_fence = 0U;
    for (std::size_t i = 0; i < execution.batches.size(); ++i) {
        const GlyphAtlasBackendUploadBatch& batch = execution.batches[i];
        const GlyphAtlasUploadReceipt& receipt = execution.receipts[i];
        if (receipt.status != GlyphAtlasUploadReceiptStatus::Completed ||
            receipt.fence_value == 0U || receipt.fence_value <= previous_fence ||
            receipt.atlas_generation_id != batch.atlas_generation_id ||
            receipt.page_generation != batch.page_generation ||
            receipt.page_index != batch.page_index ||
            receipt.first_upload != batch.first_upload ||
            receipt.upload_count != batch.upload_count ||
            batch.first_upload > submission.uploads.size() ||
            batch.upload_count > submission.uploads.size() - batch.first_upload) {
            return false;
        }
        previous_fence = receipt.fence_value;
    }
    return execution.last_fence_value == previous_fence;
}

} // namespace zevryon::text
