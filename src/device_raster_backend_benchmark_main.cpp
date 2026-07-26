#include "device_raster_backend.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <span>
#include <vector>

namespace {
using namespace zevryon::text;

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    explicit CountingMemoryResource(std::size_t hard_limit) : hard_limit_(hard_limit) {}
    std::size_t current() const noexcept { return current_; }
    std::size_t peak() const noexcept { return peak_; }
    std::size_t hard_limit() const noexcept { return hard_limit_; }
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > hard_limit_ - current_) {
            throw std::bad_alloc();
        }
        void* p = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        current_ += bytes;
        peak_ = std::max(peak_, current_);
        return p;
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        current_ -= bytes;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t hard_limit_;
    std::size_t current_{0};
    std::size_t peak_{0};
};

void hash_u64(std::uint64_t value, std::uint64_t* hash) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        *hash ^= static_cast<std::uint8_t>((value >> shift) & 0xFFU);
        *hash *= 1'099'511'628'211ULL;
    }
}

std::uint64_t checksum(
    const DeviceGlyphRasterPlan& plan,
    const DeviceGlyphRasterSourceSet& sources,
    const GlyphAtlasSubmission& cold,
    const GlyphAtlasUploadExecution& uploads,
    const GlyphAtlasSubmission& hot) {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    hash_u64(plan.jobs.size(), &hash);
    for (const DeviceGlyphRasterJob& job : plan.jobs) {
        hash_u64(job.job_id, &hash);
        hash_u64(job.key.glyph_id, &hash);
        hash_u64(job.flags, &hash);
    }
    hash_u64(sources.sources.size(), &hash);
    hash_u64(sources.payload.size(), &hash);
    for (const GlyphRasterSourceRecord& source : sources.sources) {
        hash_u64(source.key.glyph_id, &hash);
        hash_u64(source.content_checksum, &hash);
        hash_u64(source.payload_size, &hash);
        hash_u64(source.width, &hash);
        hash_u64(source.height, &hash);
        hash_u64(static_cast<std::uint64_t>(source.format), &hash);
    }
    hash_u64(cold.uploads.size(), &hash);
    hash_u64(cold.draw_instances.size(), &hash);
    hash_u64(cold.draw_batches.size(), &hash);
    hash_u64(uploads.batches.size(), &hash);
    hash_u64(uploads.receipts.size(), &hash);
    hash_u64(uploads.last_fence_value, &hash);
    hash_u64(hot.uploads.size(), &hash);
    hash_u64(hot.draw_instances.size(), &hash);
    for (std::byte byte : sources.payload) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

GlyphRasterWorkingSet build_working_set(std::pmr::memory_resource* resource) {
    GlyphRasterWorkingSet working(resource);
    working.entries.reserve(96U);
    working.uses.reserve(320U);
    std::uint32_t first_use = 0U;
    for (std::uint32_t i = 0U; i < 96U; ++i) {
        GlyphRasterWorkingSetEntry entry;
        entry.key.font_generation_id = 7U;
        entry.key.face_id = 3U;
        entry.key.glyph_id = i + 1U;
        entry.key.x_scale = 1'024;
        entry.key.y_scale = 1'024;
        entry.key.mode = i < 64U ? GlyphRasterMode::Grayscale :
            (i < 80U ? GlyphRasterMode::Lcd : GlyphRasterMode::Color);
        entry.key.subpixel_x = entry.key.mode == GlyphRasterMode::Color ? 0U :
            static_cast<std::uint8_t>(i % 3U);
        entry.key.subpixel_y = entry.key.subpixel_x;
        entry.first_use_index = first_use;
        entry.use_count = i < 32U ? 4U : 3U;
        working.entries.push_back(entry);
        for (std::uint32_t use_index = 0U; use_index < entry.use_count; ++use_index) {
            GlyphRasterUseRecord use;
            use.viewport_inline_origin =
                static_cast<std::int64_t>((i % 4U) * 40U + use_index * 8U);
            use.viewport_baseline_origin =
                static_cast<std::int64_t>((i / 4U) * 20U + 16U);
            use.key_index = i;
            use.paint_command_index = first_use + use_index;
            use.glyph_batch_index = i;
            use.glyph_index = use_index;
            use.style_id = i < 64U ? 1U : (i < 80U ? 2U : 3U);
            use.clip_index = 0U;
            use.source_line_index = 8'184U + i / 4U;
            working.uses.push_back(use);
        }
        first_use += entry.use_count;
    }
    assert(working.uses.size() == 320U);
    return working;
}

struct RunResult {
    double milliseconds{0.0};
    std::uint64_t source_payload_bytes{0};
    std::uint64_t cold_uploads{0};
    std::uint64_t cold_draw_instances{0};
    std::uint64_t cold_draw_batches{0};
    std::uint64_t backend_upload_batches{0};
    std::uint64_t hot_jobs{0};
    std::uint64_t hot_uploads{0};
    std::uint64_t hot_draw_instances{0};
    std::uint64_t checksum{0};
    std::size_t output_current{0};
    std::size_t output_peak{0};
    GlyphAtlasCacheStats cache_stats;
};

RunResult run_once() {
    constexpr std::size_t kOutputHardLimit = 4U << 20U;
    CountingMemoryResource output_resource(kOutputHardLimit);
    GlyphRasterWorkingSet working = build_working_set(&output_resource);
    std::array<std::byte, 256> font_bytes{};
    for (std::size_t i = 0; i < font_bytes.size(); ++i) {
        font_bytes[i] = static_cast<std::byte>(i);
    }
    DeviceRasterFaceSource face;
    face.font_generation_id = 7U;
    face.face_id = 3U;
    face.resource_id = 99U;
    face.bytes = font_bytes;

    DeviceGlyphRasterPlan plan(&output_resource);
    DeviceGlyphRasterPlanRequest plan_request;
    plan_request.working_set = &working;
    plan_request.face_sources = std::span<const DeviceRasterFaceSource>(&face, 1U);
    plan_request.queue_generation = 41U;
    plan_request.atlas_generation_id = 1U;
    plan_request.limits.maximum_jobs = 96U;
    DeviceGlyphRasterPlanStats plan_stats;
    DeviceGlyphRasterPlanError plan_error;

    ReferenceDeviceGlyphRasterBackend raster_backend;
    DeviceGlyphRasterSourceSet sources(&output_resource);
    DeviceGlyphRasterExecutionRequest raster_request;
    raster_request.plan = &plan;
    raster_request.face_sources = std::span<const DeviceRasterFaceSource>(&face, 1U);
    raster_request.expected_queue_generation = 41U;
    raster_request.limits.maximum_sources = 96U;
    raster_request.limits.maximum_payload_bytes = 1U << 20U;
    DeviceGlyphRasterExecutionStats raster_stats;
    DeviceGlyphRasterExecutionError raster_error;

    GlyphAtlasConfig config;
    config.page_width = 256U;
    config.page_height = 256U;
    config.maximum_pages = 3U;
    config.maximum_entries = 128U;
    config.slot_padding = 1U;
    GlyphAtlasCache cache(config, 64U << 10U);
    GlyphAtlasSubmission cold(&output_resource);
    GlyphAtlasSubmission hot(&output_resource);
    GlyphAtlasSubmissionError submission_error;
    GlyphAtlasSubmissionStats cold_stats;
    GlyphAtlasSubmissionStats hot_stats;
    ReferenceGlyphAtlasUploadBackend upload_backend;
    GlyphAtlasUploadExecution upload_execution(&output_resource);
    GlyphAtlasUploadExecutionStats upload_stats;
    GlyphAtlasUploadExecutionError upload_error;

    const auto start = std::chrono::steady_clock::now();
    assert(build_device_glyph_raster_plan(
        plan_request, &plan, &plan_stats, &plan_error));
    assert(execute_device_glyph_raster_plan(
        raster_request, &raster_backend, &sources, &raster_stats, &raster_error));

    GlyphAtlasSubmissionRequest cold_request;
    cold_request.working_set = &working;
    cold_request.raster_sources = sources.sources;
    cold_request.raster_payload = sources.payload;
    cold_request.limits.maximum_uploads = 96U;
    cold_request.limits.maximum_upload_bytes = 1U << 20U;
    cold_request.limits.maximum_draw_instances = 320U;
    cold_request.limits.maximum_draw_batches = 320U;
    assert(prepare_glyph_atlas_submission(
        cold_request, &cache, &cold, &cold_stats, &submission_error));

    GlyphAtlasUploadExecutionRequest upload_request;
    upload_request.submission = &cold;
    upload_request.cache = &cache;
    upload_request.raster_payload = sources.payload;
    upload_request.limits.maximum_batches = 96U;
    upload_request.limits.maximum_upload_bytes = 1U << 20U;
    assert(execute_glyph_atlas_uploads(upload_request, &upload_backend,
        &upload_execution, &upload_stats, &upload_error));
    assert(glyph_atlas_upload_execution_is_current(cache, cold, upload_execution));

    std::vector<GlyphRasterKey> resident;
    resident.reserve(working.entries.size());
    for (const GlyphRasterWorkingSetEntry& entry : working.entries) {
        resident.push_back(entry.key);
    }
    DeviceGlyphRasterPlan hot_plan(&output_resource);
    DeviceGlyphRasterPlanRequest hot_plan_request = plan_request;
    hot_plan_request.resident_keys = resident;
    hot_plan_request.queue_generation = 42U;
    DeviceGlyphRasterPlanStats hot_plan_stats;
    assert(build_device_glyph_raster_plan(
        hot_plan_request, &hot_plan, &hot_plan_stats, &plan_error));

    GlyphAtlasSubmissionRequest hot_request;
    hot_request.working_set = &working;
    hot_request.limits = cold_request.limits;
    assert(prepare_glyph_atlas_submission(
        hot_request, &cache, &hot, &hot_stats, &submission_error));
    const auto stop = std::chrono::steady_clock::now();

    RunResult result;
    result.milliseconds =
        std::chrono::duration<double, std::milli>(stop - start).count();
    result.source_payload_bytes = sources.payload.size();
    result.cold_uploads = cold.uploads.size();
    result.cold_draw_instances = cold.draw_instances.size();
    result.cold_draw_batches = cold.draw_batches.size();
    result.backend_upload_batches = upload_execution.batches.size();
    result.hot_jobs = hot_plan.jobs.size();
    result.hot_uploads = hot.uploads.size();
    result.hot_draw_instances = hot.draw_instances.size();
    result.checksum = checksum(plan, sources, cold, upload_execution, hot);
    result.output_current = output_resource.current();
    result.output_peak = output_resource.peak();
    result.cache_stats = cache.snapshot();
    return result;
}

} // namespace

int main() {
    constexpr std::size_t kIterations = 256U;
    std::vector<double> samples;
    samples.reserve(kIterations);
    RunResult reference;
    for (std::size_t i = 0; i < kIterations; ++i) {
        RunResult current = run_once();
        if (i == 0U) {
            reference = current;
        } else {
            assert(current.source_payload_bytes == reference.source_payload_bytes);
            assert(current.cold_uploads == reference.cold_uploads);
            assert(current.cold_draw_instances == reference.cold_draw_instances);
            assert(current.cold_draw_batches == reference.cold_draw_batches);
            assert(current.backend_upload_batches == reference.backend_upload_batches);
            assert(current.hot_jobs == 0U);
            assert(current.hot_uploads == 0U);
            assert(current.hot_draw_instances == reference.hot_draw_instances);
            assert(current.checksum == reference.checksum);
            assert(current.output_current == reference.output_current);
            assert(current.output_peak == reference.output_peak);
        }
        samples.push_back(current.milliseconds);
    }
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double p) {
        const std::size_t index = static_cast<std::size_t>(
            p * static_cast<double>(samples.size() - 1U));
        return samples[index];
    };
    std::cout << "{\n"
              << "  \"schema\": \"zevryon.device-raster-backend-benchmark.v1\",\n"
              << "  \"input_document_lines\": 16384,\n"
              << "  \"input_projected_lines\": 80,\n"
              << "  \"input_unique_keys\": 96,\n"
              << "  \"input_glyph_uses\": 320,\n"
              << "  \"cold_jobs\": 96,\n"
              << "  \"source_records\": 96,\n"
              << "  \"source_payload_bytes\": " << reference.source_payload_bytes << ",\n"
              << "  \"cold_uploads\": " << reference.cold_uploads << ",\n"
              << "  \"cold_draw_instances\": " << reference.cold_draw_instances << ",\n"
              << "  \"cold_draw_batches\": " << reference.cold_draw_batches << ",\n"
              << "  \"backend_upload_batches\": " << reference.backend_upload_batches << ",\n"
              << "  \"hot_jobs\": " << reference.hot_jobs << ",\n"
              << "  \"hot_uploads\": " << reference.hot_uploads << ",\n"
              << "  \"hot_draw_instances\": " << reference.hot_draw_instances << ",\n"
              << "  \"output_current_bytes\": " << reference.output_current << ",\n"
              << "  \"output_peak_bytes\": " << reference.output_peak << ",\n"
              << "  \"cache_current_bytes\": " << reference.cache_stats.metadata.current_bytes << ",\n"
              << "  \"cache_peak_bytes\": " << reference.cache_stats.metadata.peak_bytes << ",\n"
              << "  \"cache_pages\": " << reference.cache_stats.page_count << ",\n"
              << "  \"cache_entries\": " << reference.cache_stats.entry_count << ",\n"
              << "  \"checksum\": " << reference.checksum << ",\n"
              << "  \"p50_ms\": " << percentile(0.50) << ",\n"
              << "  \"p95_ms\": " << percentile(0.95) << ",\n"
              << "  \"p99_ms\": " << percentile(0.99) << ",\n"
              << "  \"maximum_ms\": " << samples.back() << "\n"
              << "}\n";
    return 0;
}
