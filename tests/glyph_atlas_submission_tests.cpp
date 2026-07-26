#include "glyph_atlas_submission.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace zevryon::text;

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

std::uint64_t checksum(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const std::byte value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

class CappedResource final : public std::pmr::memory_resource {
public:
    explicit CappedResource(std::size_t cap) : cap_(cap) {}

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > cap_ - used_) {
            throw std::bad_alloc();
        }
        void* pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        used_ += bytes;
        return pointer;
    }

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        used_ -= bytes;
    }

    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t cap_{0};
    std::size_t used_{0};
};

struct Fixture final {
    MultiRunShapedText shaped;
    TextPaintCommandStream paint;
    std::array<std::uint64_t, 2> generations{11U, 12U};
    std::array<GlyphRasterConfig, 2> raster_configs{};

    Fixture()
        : shaped(std::pmr::get_default_resource()),
          paint(std::pmr::get_default_resource()) {
        shaped.segments.emplace_back(std::pmr::get_default_resource());
        MultiRunShapedSegment& ltr = shaped.segments.back();
        ltr.run.face_id = 1U;
        ltr.run.direction = ShapingDirection::LeftToRight;
        ltr.glyphs.direction = ShapingDirection::LeftToRight;
        ltr.glyphs.x_scale = 64;
        ltr.glyphs.y_scale = 64;
        ltr.glyphs.glyphs.push_back({10U, 0U, 10, 0, 1, 2, 0U});
        ltr.glyphs.glyphs.push_back({11U, 1U, 12, 0, 2, -1, 0U});

        shaped.segments.emplace_back(std::pmr::get_default_resource());
        MultiRunShapedSegment& rtl = shaped.segments.back();
        rtl.run.face_id = 2U;
        rtl.run.direction = ShapingDirection::RightToLeft;
        rtl.glyphs.direction = ShapingDirection::RightToLeft;
        rtl.glyphs.x_scale = 64;
        rtl.glyphs.y_scale = 64;
        rtl.glyphs.glyphs.push_back({20U, 3U, -10, 0, -1, 0, 0U});
        rtl.glyphs.glyphs.push_back({21U, 2U, -10, 0, 0, 1, 0U});

        paint.commands.push_back(
            {TextPaintCommandKind::SelectionRect, 0U, 0U, 0U});

        TextPaintGlyphBatch ltr_batch;
        ltr_batch.viewport_inline_origin = 100;
        ltr_batch.viewport_baseline = 50;
        ltr_batch.segment_index = 0U;
        ltr_batch.first_glyph = 0U;
        ltr_batch.glyph_count = 2U;
        ltr_batch.style_id = 7U;
        ltr_batch.face_id = 1U;
        ltr_batch.x_scale = 64;
        ltr_batch.y_scale = 64;
        ltr_batch.source_line_index = 3U;
        paint.glyph_batches.push_back(ltr_batch);
        paint.commands.push_back(
            {TextPaintCommandKind::GlyphBatch, 0U, 0U, 0U});

        TextPaintGlyphBatch rtl_batch;
        rtl_batch.viewport_inline_origin = 200;
        rtl_batch.viewport_baseline = 60;
        rtl_batch.segment_index = 1U;
        rtl_batch.first_glyph = 0U;
        rtl_batch.glyph_count = 2U;
        rtl_batch.style_id = 7U;
        rtl_batch.face_id = 2U;
        rtl_batch.x_scale = 64;
        rtl_batch.y_scale = 64;
        rtl_batch.source_line_index = 4U;
        rtl_batch.flags = kTextPaintGlyphBatchRtl;
        paint.glyph_batches.push_back(rtl_batch);
        paint.commands.push_back(
            {TextPaintCommandKind::GlyphBatch, 1U, 0U, 0U});
        paint.commands.push_back({TextPaintCommandKind::CaretRect, 0U, 0U, 0U});

        raster_configs[1].mode = GlyphRasterMode::Lcd;
    }

    GlyphRasterWorkingSetRequest request() const {
        return {
            &paint,
            &shaped,
            generations,
            raster_configs,
            {16U, 16U}};
    }
};

struct RasterBundle final {
    std::vector<GlyphRasterSourceRecord> sources;
    std::vector<std::byte> payload;
};

RasterBundle make_sources(const GlyphRasterWorkingSet& working_set) {
    RasterBundle bundle;
    bundle.sources.reserve(working_set.entries.size());
    for (std::size_t index = 0U; index < working_set.entries.size(); ++index) {
        GlyphRasterSourceRecord source;
        source.key = working_set.entries[index].key;
        source.width = 8U;
        source.height = 8U;
        source.bearing_x = 1;
        source.bearing_y = 6;
        source.format = source.key.mode == GlyphRasterMode::Lcd
            ? GlyphRasterFormat::LcdRgb8
            : GlyphRasterFormat::Alpha8;
        const std::uint32_t bytes_per_pixel =
            source.format == GlyphRasterFormat::LcdRgb8 ? 3U : 1U;
        source.row_bytes = source.width * bytes_per_pixel;
        source.payload_offset = bundle.payload.size();
        source.payload_size =
            static_cast<std::uint64_t>(source.row_bytes) * source.height;
        for (std::uint64_t byte = 0U; byte < source.payload_size; ++byte) {
            bundle.payload.push_back(static_cast<std::byte>(
                (index * 17U + static_cast<std::size_t>(byte)) & 0xffU));
        }
        source.content_checksum = checksum(
            std::span<const std::byte>(bundle.payload).subspan(
                static_cast<std::size_t>(source.payload_offset),
                static_cast<std::size_t>(source.payload_size)));
        bundle.sources.push_back(source);
    }
    return bundle;
}

GlyphRasterWorkingSet one_key_working_set(
    const GlyphRasterWorkingSetEntry& source_entry,
    const GlyphRasterUseRecord& source_use) {
    GlyphRasterWorkingSet output;
    GlyphRasterWorkingSetEntry entry = source_entry;
    entry.first_use_index = 0U;
    entry.use_count = 1U;
    output.entries.push_back(entry);
    GlyphRasterUseRecord use = source_use;
    use.key_index = 0U;
    output.uses.push_back(use);
    return output;
}

bool test_working_set_and_cold_hot_cache() {
    Fixture fixture;
    GlyphRasterWorkingSet working_set;
    GlyphRasterWorkingSetStats working_stats;
    GlyphRasterWorkingSetError working_error;
    if (!require(
            build_glyph_raster_working_set(
                fixture.request(),
                &working_set,
                &working_stats,
                &working_error),
            working_error.message) ||
        !require(working_set.entries.size() == 4U, "four unique raster keys") ||
        !require(working_set.uses.size() == 4U, "four retained glyph uses") ||
        !require(
            working_set.uses[0].viewport_inline_origin == 101,
            "first LTR glyph offset applied") ||
        !require(
            working_set.uses[1].viewport_inline_origin == 112,
            "second LTR pen progression applied") ||
        !require(
            working_set.uses[2].viewport_inline_origin == 199,
            "first RTL glyph offset applied") ||
        !require(
            working_set.uses[3].viewport_inline_origin == 190,
            "second RTL negative advance applied") ||
        !require(working_stats.rtl_uses == 2U, "RTL uses classified")) {
        return false;
    }

    RasterBundle rasters = make_sources(working_set);
    GlyphAtlasCache cache({64U, 64U, 2U, 16U, 1U, 0U}, 1U << 20U);
    GlyphAtlasSubmission cold;
    GlyphAtlasSubmissionStats stats;
    GlyphAtlasSubmissionError error;
    GlyphAtlasSubmissionRequest cold_request{
        &working_set,
        rasters.sources,
        rasters.payload,
        {16U, 1U << 20U, 16U, 16U}};
    if (!require(
            prepare_glyph_atlas_submission(
                cold_request,
                &cache,
                &cold,
                &stats,
                &error),
            error.message) ||
        !require(cold.uploads.size() == 4U, "cold cache uploads four glyphs") ||
        !require(
            cold.draw_instances.size() == 4U,
            "cold submission retains four draw instances") ||
        !require(cold.draw_batches.size() == 2U, "format pages split batches") ||
        !require(stats.cache_misses == 4U, "cold cache reports misses") ||
        !require(
            glyph_atlas_submission_is_current(cache, cold),
            "cold submission generations are current")) {
        return false;
    }

    GlyphAtlasSubmission hot;
    GlyphAtlasSubmissionRequest hot_request{
        &working_set,
        {},
        {},
        {0U, 0U, 16U, 16U}};
    if (!require(
            prepare_glyph_atlas_submission(
                hot_request,
                &cache,
                &hot,
                &stats,
                &error),
            error.message) ||
        !require(hot.uploads.empty(), "hot cache publishes no uploads") ||
        !require(stats.cache_hits == 4U, "hot cache reports hits") ||
        !require(
            glyph_atlas_submission_is_current(cache, cold),
            "older submission remains valid while page generations remain")) {
        return false;
    }

    cache.clear();
    return require(
        !glyph_atlas_submission_is_current(cache, cold),
        "cache clear invalidates the atlas generation");
}

bool test_empty_missing_checksum_and_collision() {
    Fixture fixture;
    GlyphRasterWorkingSet all;
    GlyphRasterWorkingSetError working_error;
    if (!build_glyph_raster_working_set(
            fixture.request(),
            &all,
            nullptr,
            &working_error)) {
        return require(false, working_error.message);
    }
    GlyphRasterWorkingSet working_set = one_key_working_set(
        all.entries[0],
        all.uses[0]);

    GlyphAtlasCache cache({16U, 16U, 1U, 4U, 0U, 0U}, 1U << 20U);
    GlyphAtlasSubmission output;
    GlyphAtlasSubmissionStats stats;
    GlyphAtlasSubmissionError error;
    GlyphAtlasSubmissionRequest missing{
        &working_set,
        {},
        {},
        {1U, 64U, 1U, 1U}};
    if (!require(
            !prepare_glyph_atlas_submission(
                missing,
                &cache,
                &output,
                &stats,
                &error),
            "cold cache miss requires raster source") ||
        !require(
            error.kind == GlyphAtlasSubmissionErrorKind::RasterSourceNotFound,
            "missing source error classified") ||
        !require(output.uploads.empty(), "failed output remains empty") ||
        !require(cache.snapshot().entry_count == 0U, "failed cache remains unchanged")) {
        return false;
    }

    std::array<std::byte, 64> payload{};
    GlyphRasterSourceRecord source;
    source.key = working_set.entries[0].key;
    source.width = 8U;
    source.height = 8U;
    source.row_bytes = 8U;
    source.payload_size = payload.size();
    source.content_checksum = checksum(payload) + 1U;
    GlyphAtlasSubmissionRequest corrupt{
        &working_set,
        std::span<const GlyphRasterSourceRecord>(&source, 1U),
        payload,
        {1U, payload.size(), 1U, 1U}};
    if (!require(
            !prepare_glyph_atlas_submission(
                corrupt,
                &cache,
                &output,
                &stats,
                &error),
            "corrupt raster checksum is rejected") ||
        !require(
            error.kind == GlyphAtlasSubmissionErrorKind::RasterChecksumMismatch,
            "checksum error classified") ||
        !require(cache.snapshot().entry_count == 0U, "checksum failure is atomic")) {
        return false;
    }

    source.content_checksum = checksum(payload);
    if (!require(
            prepare_glyph_atlas_submission(
                corrupt,
                &cache,
                &output,
                &stats,
                &error),
            error.message)) {
        return false;
    }
    GlyphRasterSourceRecord collision = source;
    ++collision.content_checksum;
    GlyphAtlasSubmissionRequest collision_request{
        &working_set,
        std::span<const GlyphRasterSourceRecord>(&collision, 1U),
        payload,
        {0U, 0U, 1U, 1U}};
    if (!require(
            !prepare_glyph_atlas_submission(
                collision_request,
                &cache,
                &output,
                &stats,
                &error),
            "resident key collision is rejected") ||
        !require(
            error.kind == GlyphAtlasSubmissionErrorKind::RasterKeyCollision,
            "key collision classified")) {
        return false;
    }

    GlyphAtlasCache empty_cache({8U, 8U, 1U, 2U, 0U, 0U}, 1U << 20U);
    GlyphRasterSourceRecord empty;
    empty.key = working_set.entries[0].key;
    empty.format = GlyphRasterFormat::Empty;
    empty.flags = kGlyphRasterSourceEmpty;
    GlyphAtlasSubmissionRequest empty_request{
        &working_set,
        std::span<const GlyphRasterSourceRecord>(&empty, 1U),
        {},
        {0U, 0U, 1U, 1U}};
    return require(
               prepare_glyph_atlas_submission(
                   empty_request,
                   &empty_cache,
                   &output,
                   &stats,
                   &error),
               error.message) &&
        require(output.uploads.empty(), "empty glyph requires no atlas upload") &&
        require(output.draw_instances.empty(), "empty glyph requires no draw") &&
        require(stats.empty_glyphs == 1U, "empty glyph use counted");
}

bool test_page_eviction_and_generation_safety() {
    Fixture fixture;
    GlyphRasterWorkingSet all;
    GlyphRasterWorkingSetError working_error;
    if (!build_glyph_raster_working_set(
            fixture.request(),
            &all,
            nullptr,
            &working_error)) {
        return require(false, working_error.message);
    }
    GlyphRasterWorkingSet first = one_key_working_set(all.entries[0], all.uses[0]);
    GlyphRasterWorkingSet second = one_key_working_set(all.entries[1], all.uses[1]);
    std::array<std::byte, 64> payload{};

    auto source_for = [&payload](const GlyphRasterWorkingSet& set) {
        GlyphRasterSourceRecord source;
        source.key = set.entries[0].key;
        source.width = 8U;
        source.height = 8U;
        source.row_bytes = 8U;
        source.payload_size = payload.size();
        source.content_checksum = checksum(payload);
        return source;
    };
    const GlyphRasterSourceRecord first_source = source_for(first);
    const GlyphRasterSourceRecord second_source = source_for(second);

    GlyphAtlasCache cache({8U, 8U, 1U, 4U, 0U, 0U}, 1U << 20U);
    GlyphAtlasSubmission first_submission;
    GlyphAtlasSubmission second_submission;
    GlyphAtlasSubmissionStats stats;
    GlyphAtlasSubmissionError error;
    GlyphAtlasSubmissionRequest first_request{
        &first,
        std::span<const GlyphRasterSourceRecord>(&first_source, 1U),
        payload,
        {1U, payload.size(), 1U, 1U}};
    GlyphAtlasSubmissionRequest second_request{
        &second,
        std::span<const GlyphRasterSourceRecord>(&second_source, 1U),
        payload,
        {1U, payload.size(), 1U, 1U}};
    return require(
               prepare_glyph_atlas_submission(
                   first_request,
                   &cache,
                   &first_submission,
                   &stats,
                   &error),
               error.message) &&
        require(
            prepare_glyph_atlas_submission(
                second_request,
                &cache,
                &second_submission,
                &stats,
                &error),
            error.message) &&
        require(stats.reset_pages == 1U, "full atlas resets one unpinned page") &&
        require(stats.evicted_entries == 1U, "page reset evicts one entry") &&
        require(
            !glyph_atlas_submission_is_current(cache, first_submission),
            "page-generation reset makes old submission stale") &&
        require(
            glyph_atlas_submission_is_current(cache, second_submission),
            "new submission references current page generation");
}

bool test_budget_failure_atomicity() {
    Fixture fixture;
    GlyphRasterWorkingSet working_set;
    GlyphRasterWorkingSetError working_error;
    if (!build_glyph_raster_working_set(
            fixture.request(),
            &working_set,
            nullptr,
            &working_error)) {
        return require(false, working_error.message);
    }
    RasterBundle rasters = make_sources(working_set);

    CappedResource output_resource(64U);
    GlyphAtlasSubmission output(&output_resource);
    GlyphAtlasCache cache({64U, 64U, 2U, 16U, 1U, 0U}, 1U << 20U);
    GlyphAtlasSubmissionStats stats;
    GlyphAtlasSubmissionError error;
    GlyphAtlasSubmissionRequest request{
        &working_set,
        rasters.sources,
        rasters.payload,
        {16U, 1U << 20U, 16U, 16U}};
    if (!require(
            !prepare_glyph_atlas_submission(
                request,
                &cache,
                &output,
                &stats,
                &error),
            "tiny output resource rejects submission") ||
        !require(output.uploads.empty(), "budget failure leaves uploads empty") ||
        !require(
            output.draw_instances.empty(),
            "budget failure leaves draw instances empty") ||
        !require(cache.snapshot().entry_count == 0U, "budget failure leaves cache empty")) {
        return false;
    }

    GlyphAtlasCache metadata_limited(
        {64U, 64U, 2U, 16U, 1U, 0U},
        sizeof(GlyphAtlasPageRecord) - 1U);
    GlyphAtlasSubmission normal_output;
    return require(
               !prepare_glyph_atlas_submission(
                   request,
                   &metadata_limited,
                   &normal_output,
                   &stats,
                   &error),
               "metadata hard limit rejects cache staging") &&
        require(
            error.kind == GlyphAtlasSubmissionErrorKind::MetadataBudgetExceeded,
            "metadata budget error classified") &&
        require(
            metadata_limited.snapshot().entry_count == 0U,
            "metadata failure leaves persistent cache unchanged");
}

} // namespace

int main() {
    return test_working_set_and_cold_hot_cache() &&
            test_empty_missing_checksum_and_collision() &&
            test_page_eviction_and_generation_safety() &&
            test_budget_failure_atomicity()
        ? 0
        : 1;
}
