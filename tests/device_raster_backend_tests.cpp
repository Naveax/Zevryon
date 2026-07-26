#include "device_raster_backend.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <new>
#include <span>
#include <vector>

namespace {
using namespace zevryon::text;

class FailingMemoryResource final : public std::pmr::memory_resource {
public:
    explicit FailingMemoryResource(std::size_t limit) : limit_(limit) {}
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > limit_ - used_) {
            throw std::bad_alloc();
        }
        void* p = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        used_ += bytes;
        return p;
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        used_ -= bytes;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t limit_;
    std::size_t used_{0};
};

bool key_less(const GlyphRasterKey& a, const GlyphRasterKey& b) {
    return std::tie(a.font_generation_id, a.face_id, a.glyph_id, a.x_scale,
               a.y_scale, a.mode, a.subpixel_x, a.subpixel_y, a.reserved) <
        std::tie(b.font_generation_id, b.face_id, b.glyph_id, b.x_scale,
               b.y_scale, b.mode, b.subpixel_x, b.subpixel_y, b.reserved);
}

std::uint64_t fnv1a64(std::span<const std::byte> bytes) {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (std::byte value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

GlyphRasterKey make_key(
    std::uint32_t glyph_id,
    GlyphRasterMode mode,
    std::uint8_t phase = 0U) {
    GlyphRasterKey key;
    key.font_generation_id = 7U;
    key.face_id = 3U;
    key.glyph_id = glyph_id;
    key.x_scale = 1'024;
    key.y_scale = 1'024;
    key.mode = mode;
    key.subpixel_x = phase;
    key.subpixel_y = phase;
    key.reserved = 0U;
    return key;
}

GlyphRasterWorkingSet make_working_set() {
    GlyphRasterWorkingSet working;
    const std::array<GlyphRasterKey, 4> keys{
        make_key(1U, GlyphRasterMode::Grayscale),
        make_key(2U, GlyphRasterMode::Lcd),
        make_key(3U, GlyphRasterMode::Color),
        make_key(29U, GlyphRasterMode::Grayscale)};
    std::uint32_t use_index = 0U;
    for (std::uint32_t i = 0U; i < keys.size(); ++i) {
        GlyphRasterWorkingSetEntry entry;
        entry.key = keys[i];
        entry.first_use_index = use_index;
        entry.use_count = 1U;
        working.entries.push_back(entry);

        GlyphRasterUseRecord use;
        use.viewport_inline_origin = static_cast<std::int64_t>(i * 20U);
        use.viewport_baseline_origin = 50;
        use.key_index = i;
        use.style_id = i < 2U ? 1U : 2U;
        use.clip_index = 0U;
        working.uses.push_back(use);
        ++use_index;
    }
    return working;
}

DeviceRasterFaceSource make_face(std::span<const std::byte> bytes) {
    DeviceRasterFaceSource face;
    face.font_generation_id = 7U;
    face.face_id = 3U;
    face.resource_id = 99U;
    face.bytes = bytes;
    return face;
}

void test_cold_hot_plan_and_reference_execution() {
    std::array<std::byte, 64> bytes{};
    const DeviceRasterFaceSource face = make_face(bytes);
    GlyphRasterWorkingSet working = make_working_set();

    std::array<GlyphRasterKey, 1> resident{working.entries[1].key};
    DeviceGlyphRasterPlanRequest request;
    request.working_set = &working;
    request.resident_keys = resident;
    request.face_sources = std::span<const DeviceRasterFaceSource>(&face, 1U);
    request.queue_generation = 5U;
    request.atlas_generation_id = 9U;
    request.limits.maximum_jobs = 8U;

    DeviceGlyphRasterPlan plan;
    DeviceGlyphRasterPlanStats stats;
    DeviceGlyphRasterPlanError error;
    assert(build_device_glyph_raster_plan(request, &plan, &stats, &error));
    assert(plan.jobs.size() == 3U);
    assert(stats.cache_hits == 1U);
    assert(stats.cold_jobs == 3U);
    assert(stats.grayscale_jobs == 2U);
    assert(stats.color_jobs == 1U);

    ReferenceDeviceGlyphRasterBackend backend;
    DeviceGlyphRasterExecutionRequest execution_request;
    execution_request.plan = &plan;
    execution_request.face_sources = std::span<const DeviceRasterFaceSource>(&face, 1U);
    execution_request.expected_queue_generation = 5U;
    execution_request.limits.maximum_sources = 8U;
    execution_request.limits.maximum_payload_bytes = 1U << 20U;

    DeviceGlyphRasterSourceSet sources;
    DeviceGlyphRasterExecutionStats execution_stats;
    DeviceGlyphRasterExecutionError execution_error;
    assert(execute_device_glyph_raster_plan(
        execution_request, &backend, &sources, &execution_stats, &execution_error));
    assert(sources.sources.size() == 3U);
    assert(execution_stats.empty_glyphs == 1U);
    assert(execution_stats.grayscale_sources == 1U);
    assert(execution_stats.color_sources == 1U);
    for (const GlyphRasterSourceRecord& source : sources.sources) {
        const auto payload = std::span<const std::byte>(sources.payload.data(), sources.payload.size())
            .subspan(static_cast<std::size_t>(source.payload_offset),
                static_cast<std::size_t>(source.payload_size));
        assert(source.content_checksum == fnv1a64(payload));
    }

    execution_request.expected_queue_generation = 6U;
    DeviceGlyphRasterSourceSet stale;
    assert(!execute_device_glyph_raster_plan(
        execution_request, &backend, &stale, nullptr, &execution_error));
    assert(execution_error.kind ==
        DeviceGlyphRasterExecutionErrorKind::StaleQueueGeneration);
    assert(stale.sources.empty());
}

void test_color_reject_and_resident_validation() {
    std::array<std::byte, 16> bytes{};
    const DeviceRasterFaceSource face = make_face(bytes);
    GlyphRasterWorkingSet working = make_working_set();
    DeviceGlyphRasterPlanRequest request;
    request.working_set = &working;
    request.face_sources = std::span<const DeviceRasterFaceSource>(&face, 1U);
    request.queue_generation = 1U;
    request.atlas_generation_id = 1U;
    request.limits.maximum_jobs = 8U;
    request.policy.color_policy = DeviceColorGlyphPolicy::Reject;
    DeviceGlyphRasterPlan plan;
    DeviceGlyphRasterPlanError error;
    assert(!build_device_glyph_raster_plan(request, &plan, nullptr, &error));
    assert(error.kind == DeviceGlyphRasterPlanErrorKind::InvalidInput);

    request.policy.color_policy = DeviceColorGlyphPolicy::BgraFallback;
    std::array<GlyphRasterKey, 2> unsorted{
        working.entries[2].key, working.entries[0].key};
    request.resident_keys = unsorted;
    assert(!build_device_glyph_raster_plan(request, &plan, nullptr, &error));
    assert(error.kind == DeviceGlyphRasterPlanErrorKind::InvalidInput);
}

void test_atlas_integration_and_upload_fences() {
    std::array<std::byte, 64> bytes{};
    const DeviceRasterFaceSource face = make_face(bytes);
    GlyphRasterWorkingSet working = make_working_set();

    DeviceGlyphRasterPlanRequest plan_request;
    plan_request.working_set = &working;
    plan_request.face_sources = std::span<const DeviceRasterFaceSource>(&face, 1U);
    plan_request.queue_generation = 3U;
    plan_request.atlas_generation_id = 1U;
    plan_request.limits.maximum_jobs = 8U;
    DeviceGlyphRasterPlan plan;
    DeviceGlyphRasterPlanError plan_error;
    assert(build_device_glyph_raster_plan(
        plan_request, &plan, nullptr, &plan_error));

    ReferenceDeviceGlyphRasterBackend raster_backend;
    DeviceGlyphRasterExecutionRequest raster_request;
    raster_request.plan = &plan;
    raster_request.face_sources = std::span<const DeviceRasterFaceSource>(&face, 1U);
    raster_request.expected_queue_generation = 3U;
    raster_request.limits.maximum_sources = 8U;
    raster_request.limits.maximum_payload_bytes = 1U << 20U;
    DeviceGlyphRasterSourceSet sources;
    DeviceGlyphRasterExecutionError raster_error;
    assert(execute_device_glyph_raster_plan(
        raster_request, &raster_backend, &sources, nullptr, &raster_error));

    GlyphAtlasConfig config;
    config.page_width = 128U;
    config.page_height = 128U;
    config.maximum_pages = 3U;
    config.maximum_entries = 16U;
    config.slot_padding = 1U;
    GlyphAtlasCache cache(config, 1U << 20U);
    GlyphAtlasSubmissionRequest submission_request;
    submission_request.working_set = &working;
    submission_request.raster_sources = sources.sources;
    submission_request.raster_payload = sources.payload;
    submission_request.limits.maximum_uploads = 8U;
    submission_request.limits.maximum_upload_bytes = 1U << 20U;
    submission_request.limits.maximum_draw_instances = 8U;
    submission_request.limits.maximum_draw_batches = 8U;
    GlyphAtlasSubmission submission;
    GlyphAtlasSubmissionError submission_error;
    assert(prepare_glyph_atlas_submission(
        submission_request, &cache, &submission, nullptr, &submission_error));
    assert(submission.uploads.size() == 3U);
    assert(submission.draw_instances.size() == 3U);

    GlyphAtlasUploadExecutionRequest upload_request;
    upload_request.submission = &submission;
    upload_request.cache = &cache;
    upload_request.raster_payload = sources.payload;
    upload_request.limits.maximum_batches = 8U;
    upload_request.limits.maximum_upload_bytes = 1U << 20U;
    ReferenceGlyphAtlasUploadBackend upload_backend;
    GlyphAtlasUploadExecution execution;
    GlyphAtlasUploadExecutionStats upload_stats;
    GlyphAtlasUploadExecutionError upload_error;
    assert(execute_glyph_atlas_uploads(
        upload_request, &upload_backend, &execution, &upload_stats, &upload_error));
    assert(execution.receipts.size() == execution.batches.size());
    assert(execution.last_fence_value != 0U);
    assert(glyph_atlas_upload_execution_is_current(cache, submission, execution));
    cache.clear();
    assert(!glyph_atlas_upload_execution_is_current(cache, submission, execution));
}

void test_output_budget_failure_is_atomic() {
    std::array<std::byte, 64> bytes{};
    const DeviceRasterFaceSource face = make_face(bytes);
    GlyphRasterWorkingSet working = make_working_set();
    DeviceGlyphRasterPlanRequest request;
    request.working_set = &working;
    request.face_sources = std::span<const DeviceRasterFaceSource>(&face, 1U);
    request.queue_generation = 2U;
    request.atlas_generation_id = 1U;
    request.limits.maximum_jobs = 8U;

    FailingMemoryResource failing(1U);
    DeviceGlyphRasterPlan plan(&failing);
    DeviceGlyphRasterPlanError error;
    assert(!build_device_glyph_raster_plan(request, &plan, nullptr, &error));
    assert(error.kind == DeviceGlyphRasterPlanErrorKind::OutputBudgetExceeded);
    assert(plan.jobs.empty());
    assert(plan.queue_generation == 0U);
}

} // namespace

int main() {
    test_cold_hot_plan_and_reference_execution();
    test_color_reject_and_resident_validation();
    test_atlas_integration_and_upload_fences();
    test_output_budget_failure_is_atomic();
    std::cout << "device-raster-backend-tests: PASS\n";
    return 0;
}
