#include "glyph_atlas_submission.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <span>
#include <vector>

namespace {
using namespace zevryon;
using namespace zevryon::text;

constexpr std::uint32_t kDocumentLines = 16'384U;
constexpr std::uint32_t kProjectedLines = 80U;
constexpr std::uint32_t kUsesPerLine = 4U;
constexpr std::uint32_t kInputUses = kProjectedLines * kUsesPerLine;
constexpr std::uint32_t kUniqueGlyphs = 96U;
constexpr std::uint32_t kExpectedDrawBatches = 80U;
constexpr std::uint64_t kExpectedRasterBytes = 16'896U;
constexpr std::size_t kExpectedOutputCurrentBytes = 43'008U;
constexpr std::size_t kExpectedOutputPeakBytes = 58'368U;
constexpr std::size_t kExpectedCacheCurrentBytes = 9'360U;
constexpr std::size_t kExpectedCachePeakBytes = 18'723U;
constexpr std::uint32_t kMeasuredIterations = 300U;
constexpr std::uint64_t kExpectedChecksum = 5'598'703'025'695'070'182ULL;

void mix(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (unsigned int byte = 0U; byte < 8U; ++byte) {
        *hash ^= (value >> (byte * 8U)) & 0xffU;
        *hash *= 1'099'511'628'211ULL;
    }
}

std::uint64_t payload_checksum(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const std::byte value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

double percentile(const std::vector<double>& values, double fraction) {
    const double rank = std::ceil(fraction * static_cast<double>(values.size()));
    const std::size_t index = static_cast<std::size_t>(
        std::max(1.0, rank) - 1.0);
    return values[std::min(index, values.size() - 1U)];
}

struct Fixture final {
    MultiRunShapedText shaped;
    TextPaintCommandStream paint;
    std::array<std::uint64_t, kUniqueGlyphs> generations{};
    std::array<GlyphRasterConfig, kUniqueGlyphs> raster_configs{};

    Fixture() {
        shaped.segments.reserve(kUniqueGlyphs);
        for (std::uint32_t segment_index = 0U;
             segment_index < kUniqueGlyphs;
             ++segment_index) {
            shaped.segments.emplace_back(std::pmr::get_default_resource());
            MultiRunShapedSegment& segment = shaped.segments.back();
            segment.run.face_id = 10U + (segment_index % 3U);
            segment.run.direction = (segment_index % 2U) == 0U
                ? ShapingDirection::LeftToRight
                : ShapingDirection::RightToLeft;
            segment.glyphs.direction = segment.run.direction;
            segment.glyphs.x_scale = 64;
            segment.glyphs.y_scale = 64;
            const std::int32_t advance = segment.run.direction ==
                    ShapingDirection::LeftToRight
                ? 8
                : -8;
            segment.glyphs.glyphs.push_back({
                1'000U + segment_index,
                segment_index,
                advance,
                0,
                static_cast<std::int32_t>(segment_index % 3U) - 1,
                static_cast<std::int32_t>(segment_index % 2U),
                0U});
            generations[segment_index] = 700U + (segment_index % 4U);
            GlyphRasterConfig& config = raster_configs[segment_index];
            config.mode = segment_index < 64U
                ? GlyphRasterMode::Grayscale
                : (segment_index < 80U
                    ? GlyphRasterMode::Lcd
                    : GlyphRasterMode::Color);
            config.subpixel_x = static_cast<std::uint8_t>(segment_index % 4U);
            config.subpixel_y = static_cast<std::uint8_t>((segment_index / 4U) % 4U);
        }

        paint.commands.reserve(kInputUses);
        paint.glyph_batches.reserve(kInputUses);
        for (std::uint32_t use_index = 0U;
             use_index < kInputUses;
             ++use_index) {
            const std::uint32_t line = use_index / kUsesPerLine;
            const std::uint32_t column = use_index % kUsesPerLine;
            const std::uint32_t segment_index = use_index % kUniqueGlyphs;
            const MultiRunShapedSegment& segment =
                shaped.segments[segment_index];
            TextPaintGlyphBatch batch;
            batch.viewport_inline_origin =
                static_cast<std::int64_t>(column * 16U + 8U);
            batch.viewport_baseline =
                static_cast<std::int64_t>(line * 20U + 15U);
            batch.segment_index = segment_index;
            batch.glyph_count = 1U;
            batch.style_id = line % 4U;
            batch.face_id = segment.run.face_id;
            batch.x_scale = segment.glyphs.x_scale;
            batch.y_scale = segment.glyphs.y_scale;
            batch.source_line_index = 8'192U + line;
            if (segment.run.direction == ShapingDirection::RightToLeft) {
                batch.flags = kTextPaintGlyphBatchRtl;
            }
            paint.glyph_batches.push_back(batch);
            paint.commands.push_back({
                TextPaintCommandKind::GlyphBatch,
                use_index,
                0U,
                0U});
        }
    }

    GlyphRasterWorkingSetRequest request() const {
        return {
            &paint,
            &shaped,
            generations,
            raster_configs,
            {kUniqueGlyphs, kInputUses}};
    }
};

struct RasterBundle final {
    std::vector<GlyphRasterSourceRecord> sources;
    std::vector<std::byte> payload;
};

RasterBundle make_rasters(const GlyphRasterWorkingSet& working_set) {
    RasterBundle result;
    result.sources.reserve(working_set.entries.size());
    result.payload.reserve(kExpectedRasterBytes);
    for (std::size_t index = 0U; index < working_set.entries.size(); ++index) {
        GlyphRasterSourceRecord source;
        source.key = working_set.entries[index].key;
        source.width = 8U;
        source.height = 12U;
        source.bearing_x = 1;
        source.bearing_y = 10;
        source.format = source.key.mode == GlyphRasterMode::Grayscale
            ? GlyphRasterFormat::Alpha8
            : (source.key.mode == GlyphRasterMode::Lcd
                ? GlyphRasterFormat::LcdRgb8
                : GlyphRasterFormat::Bgra8);
        const std::uint32_t bytes_per_pixel =
            source.format == GlyphRasterFormat::Alpha8 ? 1U
            : (source.format == GlyphRasterFormat::LcdRgb8 ? 3U : 4U);
        source.row_bytes = source.width * bytes_per_pixel;
        source.payload_offset = result.payload.size();
        source.payload_size =
            static_cast<std::uint64_t>(source.row_bytes) * source.height;
        for (std::uint64_t byte = 0U; byte < source.payload_size; ++byte) {
            result.payload.push_back(static_cast<std::byte>(
                (index * 29U + static_cast<std::size_t>(byte)) & 0xffU));
        }
        source.content_checksum = payload_checksum(
            std::span<const std::byte>(result.payload).subspan(
                static_cast<std::size_t>(source.payload_offset),
                static_cast<std::size_t>(source.payload_size)));
        result.sources.push_back(source);
    }
    return result;
}

std::uint64_t checksum(
    const GlyphRasterWorkingSet& working_set,
    const GlyphAtlasSubmission& submission,
    const GlyphAtlasCacheStats& cache) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const GlyphRasterWorkingSetEntry& entry : working_set.entries) {
        mix(&hash, entry.key.font_generation_id);
        mix(&hash, entry.key.face_id);
        mix(&hash, entry.key.glyph_id);
        mix(&hash, static_cast<std::uint32_t>(entry.key.x_scale));
        mix(&hash, static_cast<std::uint32_t>(entry.key.y_scale));
        mix(&hash, static_cast<std::uint8_t>(entry.key.mode));
        mix(&hash, entry.key.subpixel_x);
        mix(&hash, entry.key.subpixel_y);
        mix(&hash, entry.first_use_index);
        mix(&hash, entry.use_count);
    }
    for (const GlyphRasterUseRecord& use : working_set.uses) {
        mix(&hash, static_cast<std::uint64_t>(use.viewport_inline_origin));
        mix(&hash, static_cast<std::uint64_t>(use.viewport_baseline_origin));
        mix(&hash, use.key_index);
        mix(&hash, use.style_id);
        mix(&hash, use.source_line_index);
        mix(&hash, use.flags);
    }
    mix(&hash, submission.atlas_generation_id);
    for (const GlyphAtlasDrawInstance& instance : submission.draw_instances) {
        mix(&hash, static_cast<std::uint64_t>(instance.viewport_inline_start));
        mix(&hash, static_cast<std::uint64_t>(instance.viewport_block_start));
        mix(&hash, instance.atlas_generation_id);
        mix(&hash, instance.page_generation);
        mix(&hash, instance.page_index);
        mix(&hash, instance.atlas_x);
        mix(&hash, instance.atlas_y);
        mix(&hash, instance.width);
        mix(&hash, instance.height);
        mix(&hash, instance.style_id);
        mix(&hash, instance.clip_index);
        mix(&hash, instance.working_set_key_index);
    }
    for (const GlyphAtlasDrawBatch& batch : submission.draw_batches) {
        mix(&hash, batch.page_generation);
        mix(&hash, batch.page_index);
        mix(&hash, batch.style_id);
        mix(&hash, batch.clip_index);
        mix(&hash, batch.first_instance);
        mix(&hash, batch.instance_count);
        mix(&hash, batch.flags);
    }
    mix(&hash, cache.atlas_generation_id);
    mix(&hash, cache.page_count);
    mix(&hash, cache.entry_count);
    mix(&hash, cache.reset_pages);
    mix(&hash, cache.evicted_entries);
    return hash;
}

} // namespace

int main() {
    Fixture fixture;
    core::ResourceLedger output_ledger;
    output_ledger.set_hard_limit(
        core::ResourceClass::PaintCommand,
        kExpectedOutputPeakBytes);
    core::LedgerMemoryResource output_resource(
        output_ledger,
        core::ResourceClass::PaintCommand);

    GlyphRasterWorkingSet working_set(&output_resource);
    GlyphRasterWorkingSetStats working_stats;
    GlyphRasterWorkingSetError working_error;
    if (!build_glyph_raster_working_set(
            fixture.request(),
            &working_set,
            &working_stats,
            &working_error)) {
        std::cerr << working_error.message << '\n';
        return 1;
    }
    RasterBundle rasters = make_rasters(working_set);
    if (rasters.payload.size() != kExpectedRasterBytes) {
        return 1;
    }

    GlyphAtlasCache cache(
        {128U, 128U, 3U, 128U, 1U, 0U},
        kExpectedCachePeakBytes);
    GlyphAtlasSubmission cold(&output_resource);
    GlyphAtlasSubmissionStats cold_stats;
    GlyphAtlasSubmissionError submission_error;
    const GlyphAtlasSubmissionRequest cold_request{
        &working_set,
        rasters.sources,
        rasters.payload,
        {kUniqueGlyphs, kExpectedRasterBytes, kInputUses, kExpectedDrawBatches}};
    if (!prepare_glyph_atlas_submission(
            cold_request,
            &cache,
            &cold,
            &cold_stats,
            &submission_error)) {
        std::cerr << submission_error.message << '\n';
        return 1;
    }
    if (cold.uploads.size() != kUniqueGlyphs ||
        cold.draw_instances.size() != kInputUses ||
        cold.draw_batches.size() != kExpectedDrawBatches ||
        cold_stats.upload_bytes != kExpectedRasterBytes) {
        return 1;
    }
    cold.release();

    GlyphAtlasSubmission hot(&output_resource);
    GlyphAtlasSubmissionStats hot_stats;
    const GlyphAtlasSubmissionRequest hot_request{
        &working_set,
        {},
        {},
        {0U, 0U, kInputUses, kExpectedDrawBatches}};
    std::vector<double> samples;
    samples.reserve(kMeasuredIterations);
    for (std::uint32_t iteration = 0U;
         iteration < kMeasuredIterations;
         ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        if (!prepare_glyph_atlas_submission(
                hot_request,
                &cache,
                &hot,
                &hot_stats,
                &submission_error)) {
            std::cerr << submission_error.message << '\n';
            return 1;
        }
        const auto stop = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(stop - start).count());
    }
    std::sort(samples.begin(), samples.end());

    const core::ResourceSnapshot output_snapshot = output_ledger.snapshot(
        core::ResourceClass::PaintCommand);
    const GlyphAtlasCacheStats cache_snapshot = cache.snapshot();
    const std::uint64_t output_checksum = checksum(
        working_set,
        hot,
        cache_snapshot);
    const bool output_accounting_clean = output_ledger.accounting_clean();
    const bool cache_accounting_clean =
        cache_snapshot.metadata.accounting_errors == 0U;

    std::cout
        << "{\n"
        << "  \"schema\": \"zevryon.glyph-atlas-submission-benchmark.v1\",\n"
        << "  \"input_document_lines\": " << kDocumentLines << ",\n"
        << "  \"input_projected_lines\": " << kProjectedLines << ",\n"
        << "  \"input_glyph_uses\": " << kInputUses << ",\n"
        << "  \"working_set_unique_keys\": " << working_set.entries.size() << ",\n"
        << "  \"working_set_uses\": " << working_set.uses.size() << ",\n"
        << "  \"grayscale_keys\": " << working_stats.grayscale_keys << ",\n"
        << "  \"lcd_keys\": " << working_stats.lcd_keys << ",\n"
        << "  \"color_keys\": " << working_stats.color_keys << ",\n"
        << "  \"cold_cache_misses\": " << cold_stats.cache_misses << ",\n"
        << "  \"cold_uploads\": " << cold_stats.uploads << ",\n"
        << "  \"cold_upload_bytes\": " << cold_stats.upload_bytes << ",\n"
        << "  \"hot_cache_hits\": " << hot_stats.cache_hits << ",\n"
        << "  \"hot_uploads\": " << hot.uploads.size() << ",\n"
        << "  \"draw_instances\": " << hot.draw_instances.size() << ",\n"
        << "  \"draw_batches\": " << hot.draw_batches.size() << ",\n"
        << "  \"coalesced_instances\": " << hot_stats.coalesced_instances << ",\n"
        << "  \"raster_key_bytes\": " << sizeof(GlyphRasterKey) << ",\n"
        << "  \"working_set_entry_bytes\": " << sizeof(GlyphRasterWorkingSetEntry) << ",\n"
        << "  \"working_set_use_bytes\": " << sizeof(GlyphRasterUseRecord) << ",\n"
        << "  \"source_record_bytes\": " << sizeof(GlyphRasterSourceRecord) << ",\n"
        << "  \"page_record_bytes\": " << sizeof(GlyphAtlasPageRecord) << ",\n"
        << "  \"cache_entry_bytes\": " << sizeof(GlyphAtlasCacheEntry) << ",\n"
        << "  \"upload_record_bytes\": " << sizeof(GlyphAtlasUploadRecord) << ",\n"
        << "  \"draw_instance_bytes\": " << sizeof(GlyphAtlasDrawInstance) << ",\n"
        << "  \"draw_batch_bytes\": " << sizeof(GlyphAtlasDrawBatch) << ",\n"
        << "  \"output_current_bytes\": " << output_snapshot.current_bytes << ",\n"
        << "  \"output_peak_bytes\": " << output_snapshot.peak_bytes << ",\n"
        << "  \"output_hard_limit_bytes\": " << output_snapshot.hard_limit_bytes << ",\n"
        << "  \"cache_current_bytes\": " << cache_snapshot.metadata.current_bytes << ",\n"
        << "  \"cache_peak_bytes\": " << cache_snapshot.metadata.peak_bytes << ",\n"
        << "  \"cache_hard_limit_bytes\": " << cache_snapshot.metadata.hard_limit_bytes << ",\n"
        << "  \"cache_pages\": " << cache_snapshot.page_count << ",\n"
        << "  \"cache_entries\": " << cache_snapshot.entry_count << ",\n"
        << "  \"cache_resets\": " << cache_snapshot.reset_pages << ",\n"
        << "  \"cache_evictions\": " << cache_snapshot.evicted_entries << ",\n"
        << "  \"checksum\": " << output_checksum << ",\n"
        << "  \"expected_checksum\": " << kExpectedChecksum << ",\n"
        << "  \"p50_ms\": " << percentile(samples, 0.50) << ",\n"
        << "  \"p95_ms\": " << percentile(samples, 0.95) << ",\n"
        << "  \"p99_ms\": " << percentile(samples, 0.99) << ",\n"
        << "  \"maximum_ms\": " << samples.back() << ",\n"
        << "  \"output_accounting_clean\": "
        << (output_accounting_clean ? "true" : "false") << ",\n"
        << "  \"cache_accounting_clean\": "
        << (cache_accounting_clean ? "true" : "false") << "\n"
        << "}\n";

    const bool checksum_ok = kExpectedChecksum == 0ULL ||
        output_checksum == kExpectedChecksum;
    return working_set.entries.size() == kUniqueGlyphs &&
            working_set.uses.size() == kInputUses &&
            working_stats.grayscale_keys == 64U &&
            working_stats.lcd_keys == 16U &&
            working_stats.color_keys == 16U &&
            cold_stats.cache_misses == kUniqueGlyphs &&
            cold_stats.uploads == kUniqueGlyphs &&
            cold_stats.upload_bytes == kExpectedRasterBytes &&
            hot_stats.cache_hits == kUniqueGlyphs &&
            hot.uploads.empty() &&
            hot.draw_instances.size() == kInputUses &&
            hot.draw_batches.size() == kExpectedDrawBatches &&
            hot_stats.coalesced_instances == kInputUses - kExpectedDrawBatches &&
            output_snapshot.current_bytes == kExpectedOutputCurrentBytes &&
            output_snapshot.peak_bytes == kExpectedOutputPeakBytes &&
            cache_snapshot.metadata.current_bytes == kExpectedCacheCurrentBytes &&
            cache_snapshot.metadata.peak_bytes == kExpectedCachePeakBytes &&
            cache_snapshot.page_count == 3U &&
            cache_snapshot.entry_count == kUniqueGlyphs &&
            cache_snapshot.reset_pages == 0U &&
            cache_snapshot.evicted_entries == 0U &&
            checksum_ok && output_accounting_clean && cache_accounting_clean
        ? 0
        : 1;
}
