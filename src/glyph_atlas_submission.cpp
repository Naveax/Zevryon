#include "glyph_atlas_submission.hpp"

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

bool key_less(const GlyphRasterKey& left, const GlyphRasterKey& right) noexcept {
    return std::tie(
               left.font_generation_id,
               left.face_id,
               left.glyph_id,
               left.x_scale,
               left.y_scale,
               left.mode,
               left.subpixel_x,
               left.subpixel_y) <
        std::tie(
               right.font_generation_id,
               right.face_id,
               right.glyph_id,
               right.x_scale,
               right.y_scale,
               right.mode,
               right.subpixel_x,
               right.subpixel_y);
}

bool add_signed(
    std::int64_t left,
    std::int64_t right,
    std::int64_t* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return false;
    }
    *output = left + right;
    return true;
}

bool subtract_signed(
    std::int64_t left,
    std::int64_t right,
    std::int64_t* output) noexcept {
    if (right == std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    return add_signed(left, -right, output);
}

bool add_unsigned(
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

void clear_working_set_error(GlyphRasterWorkingSetError* error) noexcept {
    if (error != nullptr) {
        error->kind = GlyphRasterWorkingSetErrorKind::None;
        error->command_index = 0U;
        error->glyph_batch_index = 0U;
        error->glyph_index = 0U;
        error->message.clear();
    }
}

bool fail_working_set(
    GlyphRasterWorkingSetErrorKind kind,
    std::size_t command_index,
    std::size_t glyph_batch_index,
    std::size_t glyph_index,
    const char* message,
    GlyphRasterWorkingSetError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->command_index = command_index;
        error->glyph_batch_index = glyph_batch_index;
        error->glyph_index = glyph_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

void clear_submission_error(GlyphAtlasSubmissionError* error) noexcept {
    if (error != nullptr) {
        error->kind = GlyphAtlasSubmissionErrorKind::None;
        error->key_index = 0U;
        error->use_index = 0U;
        error->source_index = 0U;
        error->page_index = 0U;
        error->message.clear();
    }
}

bool fail_submission(
    GlyphAtlasSubmissionErrorKind kind,
    std::size_t key_index,
    std::size_t use_index,
    std::size_t source_index,
    std::uint32_t page_index,
    const char* message,
    GlyphAtlasSubmissionError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->key_index = key_index;
        error->use_index = use_index;
        error->source_index = source_index;
        error->page_index = page_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

struct PendingUse final {
    GlyphRasterKey key;
    GlyphRasterUseRecord use;
    std::uint32_t segment_index{0};
};

static_assert(sizeof(PendingUse) == 88U);

std::uint32_t raster_bytes_per_pixel(GlyphRasterFormat format) noexcept {
    switch (format) {
        case GlyphRasterFormat::Alpha8:
            return 1U;
        case GlyphRasterFormat::LcdRgb8:
            return 3U;
        case GlyphRasterFormat::Bgra8:
            return 4U;
        case GlyphRasterFormat::Empty:
            return 0U;
    }
    return 0U;
}

bool format_matches_mode(
    GlyphRasterFormat format,
    GlyphRasterMode mode) noexcept {
    if (format == GlyphRasterFormat::Empty) {
        return true;
    }
    switch (mode) {
        case GlyphRasterMode::Grayscale:
            return format == GlyphRasterFormat::Alpha8;
        case GlyphRasterMode::Lcd:
            return format == GlyphRasterFormat::LcdRgb8;
        case GlyphRasterMode::Color:
            return format == GlyphRasterFormat::Bgra8;
    }
    return false;
}

std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const std::byte value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

using CacheEntryIterator = std::pmr::vector<GlyphAtlasCacheEntry>::iterator;
using CacheEntryConstIterator = std::pmr::vector<GlyphAtlasCacheEntry>::const_iterator;

CacheEntryIterator find_entry(
    std::pmr::vector<GlyphAtlasCacheEntry>* entries,
    const GlyphRasterKey& key) noexcept {
    return std::lower_bound(
        entries->begin(),
        entries->end(),
        key,
        [](const GlyphAtlasCacheEntry& entry, const GlyphRasterKey& value) {
            return key_less(entry.key, value);
        });
}

CacheEntryConstIterator find_entry(
    const std::pmr::vector<GlyphAtlasCacheEntry>& entries,
    const GlyphRasterKey& key) noexcept {
    return std::lower_bound(
        entries.begin(),
        entries.end(),
        key,
        [](const GlyphAtlasCacheEntry& entry, const GlyphRasterKey& value) {
            return key_less(entry.key, value);
        });
}

std::span<const GlyphRasterSourceRecord>::iterator find_source(
    std::span<const GlyphRasterSourceRecord> sources,
    const GlyphRasterKey& key) noexcept {
    return std::lower_bound(
        sources.begin(),
        sources.end(),
        key,
        [](const GlyphRasterSourceRecord& source, const GlyphRasterKey& value) {
            return key_less(source.key, value);
        });
}

bool source_basic_valid(
    const GlyphRasterSourceRecord& source,
    std::span<const std::byte> payload) noexcept {
    const bool empty =
        (source.flags & static_cast<std::uint8_t>(kGlyphRasterSourceEmpty)) != 0U;
    if (source.reserved != 0U || source.reserved2 != 0U ||
        source.key.reserved != 0U || !format_matches_mode(source.format, source.key.mode)) {
        return false;
    }
    if (empty) {
        return source.format == GlyphRasterFormat::Empty && source.width == 0U &&
            source.height == 0U && source.row_bytes == 0U &&
            source.payload_offset == 0U && source.payload_size == 0U &&
            source.content_checksum == 0U;
    }
    if (source.format == GlyphRasterFormat::Empty || source.width == 0U ||
        source.height == 0U) {
        return false;
    }
    const std::uint32_t bytes_per_pixel = raster_bytes_per_pixel(source.format);
    if (bytes_per_pixel == 0U ||
        source.width > std::numeric_limits<std::uint32_t>::max() / bytes_per_pixel) {
        return false;
    }
    const std::uint32_t minimum_row = source.width * bytes_per_pixel;
    if (source.row_bytes < minimum_row) {
        return false;
    }
    const std::uint64_t expected_size =
        static_cast<std::uint64_t>(source.row_bytes) * source.height;
    if (source.payload_size != expected_size ||
        source.payload_offset > payload.size() ||
        source.payload_size > payload.size() - source.payload_offset) {
        return false;
    }
    return true;
}

struct Placement final {
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t next_x{0};
    std::uint32_t next_y{0};
    std::uint32_t row_height{0};
};

bool plan_placement(
    const GlyphAtlasPageRecord& page,
    const GlyphAtlasConfig& config,
    std::uint32_t width,
    std::uint32_t height,
    Placement* placement) noexcept {
    if (placement == nullptr || width == 0U || height == 0U ||
        width > config.page_width || height > config.page_height) {
        return false;
    }
    std::uint32_t x = page.next_x;
    std::uint32_t y = page.next_y;
    std::uint32_t row_height = page.row_height;
    if (x != 0U && width > config.page_width - x) {
        const std::uint64_t next_row = static_cast<std::uint64_t>(y) +
            row_height + config.slot_padding;
        if (next_row > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        x = 0U;
        y = static_cast<std::uint32_t>(next_row);
        row_height = 0U;
    }
    if (y > config.page_height || height > config.page_height - y) {
        return false;
    }
    const std::uint64_t after_x =
        static_cast<std::uint64_t>(x) + width + config.slot_padding;
    placement->x = x;
    placement->y = y;
    placement->next_x = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(after_x, config.page_width));
    placement->next_y = y;
    placement->row_height = std::max(row_height, height);
    return true;
}

std::uint64_t erase_page_entries(
    std::pmr::vector<GlyphAtlasCacheEntry>* entries,
    std::uint32_t page_index) noexcept {
    const std::size_t before = entries->size();
    entries->erase(
        std::remove_if(
            entries->begin(),
            entries->end(),
            [page_index](const GlyphAtlasCacheEntry& entry) {
                return entry.page_index == page_index;
            }),
        entries->end());
    return static_cast<std::uint64_t>(before - entries->size());
}

bool reset_page(
    std::pmr::vector<GlyphAtlasPageRecord>* pages,
    std::pmr::vector<GlyphAtlasCacheEntry>* entries,
    std::uint32_t page_index,
    GlyphRasterFormat format,
    std::uint64_t epoch,
    GlyphAtlasSubmissionStats* stats,
    GlyphAtlasSubmissionError* error,
    std::size_t key_index) noexcept {
    GlyphAtlasPageRecord& page = (*pages)[page_index];
    if (page.initialized != 0U &&
        page.generation == std::numeric_limits<std::uint64_t>::max()) {
        return fail_submission(
            GlyphAtlasSubmissionErrorKind::ArithmeticOverflow,
            key_index,
            0U,
            0U,
            page_index,
            "glyph atlas page generation overflow",
            error);
    }
    const std::uint64_t removed = erase_page_entries(entries, page_index);
    const std::uint64_t next_generation =
        page.initialized == 0U ? 1U : page.generation + 1U;
    page = {};
    page.generation = next_generation;
    page.last_use_epoch = epoch;
    page.format = format;
    page.initialized = 1U;
    if (stats != nullptr) {
        stats->evicted_entries += removed;
        ++stats->reset_pages;
    }
    return true;
}

bool ensure_entry_capacity(
    const GlyphAtlasConfig& config,
    std::pmr::vector<GlyphAtlasPageRecord>* pages,
    std::pmr::vector<GlyphAtlasCacheEntry>* entries,
    std::span<const std::uint8_t> pinned_pages,
    std::uint64_t epoch,
    GlyphAtlasSubmissionStats* stats,
    GlyphAtlasSubmissionError* error,
    std::size_t key_index) noexcept {
    if (entries->size() < config.maximum_entries) {
        return true;
    }
    auto empty_candidate = entries->end();
    for (auto iterator = entries->begin(); iterator != entries->end(); ++iterator) {
        if ((iterator->flags &
             static_cast<std::uint8_t>(kGlyphAtlasCacheEntryEmpty)) != 0U &&
            iterator->last_use_epoch != epoch &&
            (empty_candidate == entries->end() ||
             iterator->last_use_epoch < empty_candidate->last_use_epoch)) {
            empty_candidate = iterator;
        }
    }
    if (empty_candidate != entries->end()) {
        entries->erase(empty_candidate);
        if (stats != nullptr) {
            ++stats->evicted_entries;
        }
        return true;
    }

    std::uint32_t candidate = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
    for (std::uint32_t index = 0U; index < pages->size(); ++index) {
        const GlyphAtlasPageRecord& page = (*pages)[index];
        if (page.initialized != 0U && pinned_pages[index] == 0U &&
            page.last_use_epoch < oldest) {
            oldest = page.last_use_epoch;
            candidate = index;
        }
    }
    if (candidate == std::numeric_limits<std::uint32_t>::max()) {
        return fail_submission(
            GlyphAtlasSubmissionErrorKind::AtlasCapacityExceeded,
            key_index,
            0U,
            0U,
            0U,
            "glyph atlas entry limit is exhausted by the current submission",
            error);
    }
    const GlyphRasterFormat format = (*pages)[candidate].format;
    return reset_page(
        pages,
        entries,
        candidate,
        format,
        epoch,
        stats,
        error,
        key_index);
}

bool acquire_page_for_source(
    const GlyphRasterSourceRecord& source,
    const GlyphAtlasConfig& config,
    std::pmr::vector<GlyphAtlasPageRecord>* pages,
    std::pmr::vector<GlyphAtlasCacheEntry>* entries,
    std::pmr::vector<std::uint8_t>* pinned_pages,
    std::uint64_t epoch,
    std::uint32_t* page_index,
    Placement* placement,
    GlyphAtlasSubmissionStats* stats,
    GlyphAtlasSubmissionError* error,
    std::size_t key_index) noexcept {
    for (std::uint32_t index = 0U; index < pages->size(); ++index) {
        const GlyphAtlasPageRecord& page = (*pages)[index];
        if (page.initialized != 0U && page.format == source.format &&
            plan_placement(page, config, source.width, source.height, placement)) {
            *page_index = index;
            return true;
        }
    }
    for (std::uint32_t index = 0U; index < pages->size(); ++index) {
        if ((*pages)[index].initialized == 0U) {
            GlyphAtlasPageRecord& page = (*pages)[index];
            page.generation = 1U;
            page.last_use_epoch = epoch;
            page.format = source.format;
            page.initialized = 1U;
            if (!plan_placement(
                    page,
                    config,
                    source.width,
                    source.height,
                    placement)) {
                return fail_submission(
                    GlyphAtlasSubmissionErrorKind::AtlasCapacityExceeded,
                    key_index,
                    0U,
                    0U,
                    index,
                    "glyph raster is larger than an atlas page",
                    error);
            }
            *page_index = index;
            return true;
        }
    }

    std::uint32_t candidate = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
    for (std::uint32_t index = 0U; index < pages->size(); ++index) {
        const GlyphAtlasPageRecord& page = (*pages)[index];
        if ((*pinned_pages)[index] == 0U && page.last_use_epoch < oldest) {
            oldest = page.last_use_epoch;
            candidate = index;
        }
    }
    if (candidate == std::numeric_limits<std::uint32_t>::max()) {
        return fail_submission(
            GlyphAtlasSubmissionErrorKind::AtlasCapacityExceeded,
            key_index,
            0U,
            0U,
            0U,
            "all glyph atlas pages are pinned by the current submission",
            error);
    }
    if (!reset_page(
            pages,
            entries,
            candidate,
            source.format,
            epoch,
            stats,
            error,
            key_index)) {
        return false;
    }
    if (!plan_placement(
            (*pages)[candidate],
            config,
            source.width,
            source.height,
            placement)) {
        return fail_submission(
            GlyphAtlasSubmissionErrorKind::AtlasCapacityExceeded,
            key_index,
            0U,
            0U,
            candidate,
            "glyph raster is larger than a reset atlas page",
            error);
    }
    *page_index = candidate;
    return true;
}

} // namespace

GlyphRasterWorkingSet::GlyphRasterWorkingSet(
    std::pmr::memory_resource* resource)
    : entries(usable_resource(resource)), uses(usable_resource(resource)) {}

std::pmr::memory_resource* GlyphRasterWorkingSet::resource() const noexcept {
    return entries.get_allocator().resource();
}

void GlyphRasterWorkingSet::release() noexcept {
    release_vector(&entries);
    release_vector(&uses);
}

const char* glyph_raster_working_set_error_kind_name(
    GlyphRasterWorkingSetErrorKind kind) noexcept {
    switch (kind) {
        case GlyphRasterWorkingSetErrorKind::None:
            return "none";
        case GlyphRasterWorkingSetErrorKind::InvalidInput:
            return "invalid-input";
        case GlyphRasterWorkingSetErrorKind::TopologyViolation:
            return "topology-violation";
        case GlyphRasterWorkingSetErrorKind::ArithmeticOverflow:
            return "arithmetic-overflow";
        case GlyphRasterWorkingSetErrorKind::WorkingSetLimitExceeded:
            return "working-set-limit-exceeded";
        case GlyphRasterWorkingSetErrorKind::OutputBudgetExceeded:
            return "output-budget-exceeded";
        case GlyphRasterWorkingSetErrorKind::AggregateOverflow:
            return "aggregate-overflow";
    }
    return "unknown";
}

bool build_glyph_raster_working_set(
    const GlyphRasterWorkingSetRequest& request,
    GlyphRasterWorkingSet* output,
    GlyphRasterWorkingSetStats* stats,
    GlyphRasterWorkingSetError* error) noexcept {
    clear_working_set_error(error);
    if (stats != nullptr) {
        *stats = {};
    }
    if (output == nullptr) {
        return fail_working_set(
            GlyphRasterWorkingSetErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            "glyph raster working-set output is null",
            error);
    }
    output->release();
    if (request.paint_stream == nullptr || request.shaped_text == nullptr) {
        return fail_working_set(
            GlyphRasterWorkingSetErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            "paint stream and shaped text are required",
            error);
    }
    const std::size_t segment_count = request.shaped_text->segments.size();
    if (request.segment_font_generation_ids.size() != segment_count ||
        (!request.segment_raster_configs.empty() &&
         request.segment_raster_configs.size() != segment_count)) {
        return fail_working_set(
            GlyphRasterWorkingSetErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            "segment generation and raster configuration tables must match shaped segments",
            error);
    }

    try {
        std::pmr::vector<PendingUse> pending(output->resource());
        std::pmr::vector<GlyphRasterKey> keys(output->resource());
        std::uint64_t glyph_references = 0U;
        bool observed_caret = false;
        for (std::size_t command_index = 0U;
             command_index < request.paint_stream->commands.size();
             ++command_index) {
            const TextPaintCommandRecord& command =
                request.paint_stream->commands[command_index];
            if (command.kind == TextPaintCommandKind::CaretRect) {
                observed_caret = true;
                continue;
            }
            if (command.kind != TextPaintCommandKind::GlyphBatch) {
                continue;
            }
            if (observed_caret ||
                command.payload_index >= request.paint_stream->glyph_batches.size()) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::TopologyViolation,
                    command_index,
                    command.payload_index,
                    0U,
                    "glyph paint command topology is invalid",
                    error);
            }
            const TextPaintGlyphBatch& batch =
                request.paint_stream->glyph_batches[command.payload_index];
            if (batch.segment_index >= segment_count) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::TopologyViolation,
                    command_index,
                    command.payload_index,
                    0U,
                    "glyph batch references an invalid shaped segment",
                    error);
            }
            const MultiRunShapedSegment& segment =
                request.shaped_text->segments[batch.segment_index];
            if (batch.glyph_count == 0U ||
                batch.first_glyph > segment.glyphs.glyphs.size() ||
                batch.glyph_count >
                    segment.glyphs.glyphs.size() - batch.first_glyph ||
                batch.face_id != segment.run.face_id ||
                batch.x_scale != segment.glyphs.x_scale ||
                batch.y_scale != segment.glyphs.y_scale ||
                (segment.glyphs.direction != ShapingDirection::LeftToRight &&
                 segment.glyphs.direction != ShapingDirection::RightToLeft)) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::TopologyViolation,
                    command_index,
                    command.payload_index,
                    batch.first_glyph,
                    "glyph batch does not match retained shaped-segment topology",
                    error);
            }
            if (request.segment_font_generation_ids[batch.segment_index] == 0U) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::InvalidInput,
                    command_index,
                    command.payload_index,
                    0U,
                    "font generation identifiers must be non-zero",
                    error);
            }
            const GlyphRasterConfig raster_config =
                request.segment_raster_configs.empty()
                ? GlyphRasterConfig{}
                : request.segment_raster_configs[batch.segment_index];
            if (raster_config.mode > GlyphRasterMode::Color ||
                raster_config.subpixel_x > 63U ||
                raster_config.subpixel_y > 63U || raster_config.reserved != 0U) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::InvalidInput,
                    command_index,
                    command.payload_index,
                    0U,
                    "glyph raster configuration is invalid",
                    error);
            }
            if (!add_unsigned(
                    glyph_references,
                    batch.glyph_count,
                    &glyph_references) ||
                glyph_references > request.limits.maximum_uses ||
                glyph_references > std::numeric_limits<std::size_t>::max()) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::WorkingSetLimitExceeded,
                    command_index,
                    command.payload_index,
                    0U,
                    "glyph raster use limit exceeded",
                    error);
            }
        }
        pending.reserve(static_cast<std::size_t>(glyph_references));
        keys.reserve(static_cast<std::size_t>(glyph_references));
        observed_caret = false;
        for (std::size_t command_index = 0U;
             command_index < request.paint_stream->commands.size();
             ++command_index) {
            const TextPaintCommandRecord& command =
                request.paint_stream->commands[command_index];
            if (command.kind == TextPaintCommandKind::CaretRect) {
                observed_caret = true;
                continue;
            }
            if (command.kind != TextPaintCommandKind::GlyphBatch) {
                continue;
            }
            if (observed_caret ||
                command.payload_index >= request.paint_stream->glyph_batches.size()) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::TopologyViolation,
                    command_index,
                    command.payload_index,
                    0U,
                    "glyph paint command topology is invalid",
                    error);
            }
            const TextPaintGlyphBatch& batch =
                request.paint_stream->glyph_batches[command.payload_index];
            if (batch.segment_index >= segment_count) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::TopologyViolation,
                    command_index,
                    command.payload_index,
                    0U,
                    "glyph batch references an invalid shaped segment",
                    error);
            }
            const MultiRunShapedSegment& segment =
                request.shaped_text->segments[batch.segment_index];
            if (batch.glyph_count == 0U ||
                batch.first_glyph > segment.glyphs.glyphs.size() ||
                batch.glyph_count >
                    segment.glyphs.glyphs.size() - batch.first_glyph ||
                batch.face_id != segment.run.face_id ||
                batch.x_scale != segment.glyphs.x_scale ||
                batch.y_scale != segment.glyphs.y_scale ||
                (segment.glyphs.direction != ShapingDirection::LeftToRight &&
                 segment.glyphs.direction != ShapingDirection::RightToLeft)) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::TopologyViolation,
                    command_index,
                    command.payload_index,
                    batch.first_glyph,
                    "glyph batch does not match retained shaped-segment topology",
                    error);
            }
            const std::uint64_t generation_id =
                request.segment_font_generation_ids[batch.segment_index];
            if (generation_id == 0U) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::InvalidInput,
                    command_index,
                    command.payload_index,
                    0U,
                    "font generation identifiers must be non-zero",
                    error);
            }
            const GlyphRasterConfig raster_config =
                request.segment_raster_configs.empty()
                ? GlyphRasterConfig{}
                : request.segment_raster_configs[batch.segment_index];
            if (raster_config.mode > GlyphRasterMode::Color ||
                raster_config.subpixel_x > 63U ||
                raster_config.subpixel_y > 63U || raster_config.reserved != 0U) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::InvalidInput,
                    command_index,
                    command.payload_index,
                    0U,
                    "glyph raster configuration is invalid",
                    error);
            }
            std::int64_t pen_inline = batch.viewport_inline_origin;
            std::int64_t pen_baseline = batch.viewport_baseline;
            for (std::uint32_t local_index = 0U;
                 local_index < batch.glyph_count;
                 ++local_index) {
                const std::uint32_t glyph_index = batch.first_glyph + local_index;
                const ShapedGlyph& glyph = segment.glyphs.glyphs[glyph_index];
                std::int64_t glyph_inline = 0;
                std::int64_t glyph_baseline = 0;
                if (!add_signed(pen_inline, glyph.x_offset, &glyph_inline) ||
                    !subtract_signed(
                        pen_baseline,
                        glyph.y_offset,
                        &glyph_baseline)) {
                    return fail_working_set(
                        GlyphRasterWorkingSetErrorKind::ArithmeticOverflow,
                        command_index,
                        command.payload_index,
                        glyph_index,
                        "glyph viewport origin overflow",
                        error);
                }
                GlyphRasterKey key;
                key.font_generation_id = generation_id;
                key.face_id = batch.face_id;
                key.glyph_id = glyph.glyph_id;
                key.x_scale = batch.x_scale;
                key.y_scale = batch.y_scale;
                key.mode = raster_config.mode;
                key.subpixel_x = raster_config.subpixel_x;
                key.subpixel_y = raster_config.subpixel_y;

                if (command_index > std::numeric_limits<std::uint32_t>::max()) {
                    return fail_working_set(
                        GlyphRasterWorkingSetErrorKind::AggregateOverflow,
                        command_index,
                        command.payload_index,
                        glyph_index,
                        "paint command index exceeds compact glyph use range",
                        error);
                }
                GlyphRasterUseRecord use;
                use.viewport_inline_origin = glyph_inline;
                use.viewport_baseline_origin = glyph_baseline;
                use.paint_command_index = static_cast<std::uint32_t>(command_index);
                use.glyph_batch_index = command.payload_index;
                use.glyph_index = glyph_index;
                use.style_id = batch.style_id;
                use.clip_index = command.clip_index;
                use.source_line_index = batch.source_line_index;
                if ((batch.flags & kTextPaintGlyphBatchRtl) != 0U) {
                    use.flags |= kGlyphRasterUseRtl;
                }
                if ((batch.flags & kTextPaintGlyphBatchBeforeViewport) != 0U) {
                    use.flags |= kGlyphRasterUseBeforeViewport;
                }
                if ((batch.flags & kTextPaintGlyphBatchAfterViewport) != 0U) {
                    use.flags |= kGlyphRasterUseAfterViewport;
                }
                pending.push_back({key, use, batch.segment_index});
                keys.push_back(key);

                if (!add_signed(pen_inline, glyph.x_advance, &pen_inline) ||
                    !subtract_signed(
                        pen_baseline,
                        glyph.y_advance,
                        &pen_baseline)) {
                    return fail_working_set(
                        GlyphRasterWorkingSetErrorKind::ArithmeticOverflow,
                        command_index,
                        command.payload_index,
                        glyph_index,
                        "glyph pen progression overflow",
                        error);
                }
            }
        }

        std::sort(keys.begin(), keys.end(), key_less);
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        if (keys.size() > request.limits.maximum_unique_keys) {
            return fail_working_set(
                GlyphRasterWorkingSetErrorKind::WorkingSetLimitExceeded,
                0U,
                0U,
                0U,
                "unique glyph raster key limit exceeded",
                error);
        }

        std::pmr::vector<GlyphRasterWorkingSetEntry> staged_entries(
            output->resource());
        std::pmr::vector<GlyphRasterUseRecord> staged_uses(output->resource());
        staged_entries.reserve(keys.size());
        staged_uses.reserve(pending.size());
        for (const GlyphRasterKey& key : keys) {
            GlyphRasterWorkingSetEntry entry;
            entry.key = key;
            entry.first_use_index = std::numeric_limits<std::uint32_t>::max();
            staged_entries.push_back(entry);
        }
        for (std::size_t index = 0U; index < pending.size(); ++index) {
            PendingUse& pending_use = pending[index];
            const auto iterator = std::lower_bound(
                keys.begin(), keys.end(), pending_use.key, key_less);
            const std::size_t key_index =
                static_cast<std::size_t>(iterator - keys.begin());
            if (key_index > std::numeric_limits<std::uint32_t>::max() ||
                index > std::numeric_limits<std::uint32_t>::max()) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::AggregateOverflow,
                    pending_use.use.paint_command_index,
                    pending_use.use.glyph_batch_index,
                    pending_use.use.glyph_index,
                    "glyph raster working-set index overflow",
                    error);
            }
            GlyphRasterWorkingSetEntry& entry = staged_entries[key_index];
            if (entry.use_count == 0U) {
                entry.first_use_index = static_cast<std::uint32_t>(index);
                entry.first_segment_index = pending_use.segment_index;
            }
            if (entry.use_count == std::numeric_limits<std::uint32_t>::max()) {
                return fail_working_set(
                    GlyphRasterWorkingSetErrorKind::AggregateOverflow,
                    pending_use.use.paint_command_index,
                    pending_use.use.glyph_batch_index,
                    pending_use.use.glyph_index,
                    "glyph raster key use count overflow",
                    error);
            }
            ++entry.use_count;
            pending_use.use.key_index = static_cast<std::uint32_t>(key_index);
            staged_uses.push_back(pending_use.use);
        }

        if (stats != nullptr) {
            stats->input_commands = request.paint_stream->commands.size();
            stats->input_glyph_batches = request.paint_stream->glyph_batches.size();
            stats->input_glyph_references = pending.size();
            stats->output_unique_keys = staged_entries.size();
            stats->output_uses = staged_uses.size();
            stats->repeated_uses = staged_uses.size() - staged_entries.size();
            for (const GlyphRasterWorkingSetEntry& entry : staged_entries) {
                switch (entry.key.mode) {
                    case GlyphRasterMode::Grayscale:
                        ++stats->grayscale_keys;
                        break;
                    case GlyphRasterMode::Lcd:
                        ++stats->lcd_keys;
                        break;
                    case GlyphRasterMode::Color:
                        ++stats->color_keys;
                        break;
                }
                stats->maximum_uses_per_key = std::max<std::uint64_t>(
                    stats->maximum_uses_per_key,
                    entry.use_count);
            }
            for (const GlyphRasterUseRecord& use : staged_uses) {
                if ((use.flags & kGlyphRasterUseRtl) != 0U) {
                    ++stats->rtl_uses;
                }
            }
        }
        output->entries.swap(staged_entries);
        output->uses.swap(staged_uses);
        return true;
    } catch (const std::bad_alloc&) {
        return fail_working_set(
            GlyphRasterWorkingSetErrorKind::OutputBudgetExceeded,
            0U,
            0U,
            0U,
            "glyph raster working-set output budget exceeded",
            error);
    } catch (...) {
        return fail_working_set(
            GlyphRasterWorkingSetErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            "unexpected glyph raster working-set failure",
            error);
    }
}

GlyphAtlasSubmission::GlyphAtlasSubmission(std::pmr::memory_resource* resource)
    : uploads(usable_resource(resource)),
      draw_instances(usable_resource(resource)),
      draw_batches(usable_resource(resource)) {}

std::pmr::memory_resource* GlyphAtlasSubmission::resource() const noexcept {
    return uploads.get_allocator().resource();
}

void GlyphAtlasSubmission::release() noexcept {
    atlas_generation_id = 0U;
    submission_epoch = 0U;
    release_vector(&uploads);
    release_vector(&draw_instances);
    release_vector(&draw_batches);
}

GlyphAtlasCache::GlyphAtlasCache(
    GlyphAtlasConfig config,
    std::size_t metadata_hard_limit) noexcept
    : metadata_resource_(ledger_, core::ResourceClass::RasterTile),
      pages_(&metadata_resource_),
      entries_(&metadata_resource_),
      config_(config) {
    ledger_.set_hard_limit(core::ResourceClass::RasterTile, metadata_hard_limit);
}

void GlyphAtlasCache::clear() noexcept {
    std::scoped_lock lock(mutex_);
    release_vector(&pages_);
    release_vector(&entries_);
    atlas_generation_id_ =
        atlas_generation_id_ == std::numeric_limits<std::uint64_t>::max()
        ? 1U
        : atlas_generation_id_ + 1U;
    use_epoch_ = 0U;
    ++clears_;
}

GlyphAtlasCacheStats GlyphAtlasCache::snapshot_locked() const noexcept {
    GlyphAtlasCacheStats stats;
    stats.metadata = ledger_.snapshot(core::ResourceClass::RasterTile);
    stats.config = config_;
    stats.atlas_generation_id = atlas_generation_id_;
    stats.use_epoch = use_epoch_;
    stats.hits = hits_;
    stats.misses = misses_;
    stats.uploads = uploads_;
    stats.evicted_entries = evicted_entries_;
    stats.reset_pages = reset_pages_;
    stats.clears = clears_;
    stats.page_count = pages_.size();
    stats.entry_count = entries_.size();
    return stats;
}

GlyphAtlasCacheStats GlyphAtlasCache::snapshot() const noexcept {
    std::scoped_lock lock(mutex_);
    return snapshot_locked();
}

const char* glyph_atlas_submission_error_kind_name(
    GlyphAtlasSubmissionErrorKind kind) noexcept {
    switch (kind) {
        case GlyphAtlasSubmissionErrorKind::None:
            return "none";
        case GlyphAtlasSubmissionErrorKind::InvalidInput:
            return "invalid-input";
        case GlyphAtlasSubmissionErrorKind::TopologyViolation:
            return "topology-violation";
        case GlyphAtlasSubmissionErrorKind::InvalidRasterSource:
            return "invalid-raster-source";
        case GlyphAtlasSubmissionErrorKind::RasterSourceNotFound:
            return "raster-source-not-found";
        case GlyphAtlasSubmissionErrorKind::RasterChecksumMismatch:
            return "raster-checksum-mismatch";
        case GlyphAtlasSubmissionErrorKind::RasterKeyCollision:
            return "raster-key-collision";
        case GlyphAtlasSubmissionErrorKind::AtlasCapacityExceeded:
            return "atlas-capacity-exceeded";
        case GlyphAtlasSubmissionErrorKind::StaleCacheEntry:
            return "stale-cache-entry";
        case GlyphAtlasSubmissionErrorKind::ArithmeticOverflow:
            return "arithmetic-overflow";
        case GlyphAtlasSubmissionErrorKind::SubmissionLimitExceeded:
            return "submission-limit-exceeded";
        case GlyphAtlasSubmissionErrorKind::MetadataBudgetExceeded:
            return "metadata-budget-exceeded";
        case GlyphAtlasSubmissionErrorKind::OutputBudgetExceeded:
            return "output-budget-exceeded";
        case GlyphAtlasSubmissionErrorKind::AggregateOverflow:
            return "aggregate-overflow";
    }
    return "unknown";
}

bool prepare_glyph_atlas_submission(
    const GlyphAtlasSubmissionRequest& request,
    GlyphAtlasCache* cache,
    GlyphAtlasSubmission* output,
    GlyphAtlasSubmissionStats* stats,
    GlyphAtlasSubmissionError* error) noexcept {
    clear_submission_error(error);
    GlyphAtlasSubmissionStats local_stats;
    GlyphAtlasSubmissionStats* effective_stats =
        stats != nullptr ? stats : &local_stats;
    *effective_stats = {};
    if (output == nullptr) {
        return fail_submission(
            GlyphAtlasSubmissionErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            0U,
            "glyph atlas submission output is null",
            error);
    }
    output->release();
    if (cache == nullptr || request.working_set == nullptr) {
        return fail_submission(
            GlyphAtlasSubmissionErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            0U,
            "glyph atlas cache and raster working set are required",
            error);
    }
    const GlyphAtlasConfig config = cache->config_;
    const std::uint64_t page_area =
        static_cast<std::uint64_t>(config.page_width) * config.page_height;
    if (config.page_width == 0U || config.page_height == 0U ||
        config.maximum_pages == 0U || config.maximum_entries == 0U ||
        config.slot_padding >= config.page_width ||
        config.slot_padding >= config.page_height ||
        page_area > std::numeric_limits<std::uint32_t>::max()) {
        return fail_submission(
            GlyphAtlasSubmissionErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            0U,
            "glyph atlas configuration is invalid",
            error);
    }
    for (std::size_t index = 0U;
         index < request.working_set->entries.size();
         ++index) {
        const GlyphRasterWorkingSetEntry& entry =
            request.working_set->entries[index];
        if (entry.key.font_generation_id == 0U ||
            entry.key.face_id == kInvalidFontFaceId || entry.use_count == 0U ||
            entry.key.reserved != 0U ||
            (index != 0U &&
             !key_less(request.working_set->entries[index - 1U].key, entry.key))) {
            return fail_submission(
                GlyphAtlasSubmissionErrorKind::TopologyViolation,
                index,
                0U,
                0U,
                0U,
                "glyph raster working-set entries are not strictly sorted and valid",
                error);
        }
    }
    for (std::size_t index = 0U; index < request.working_set->uses.size(); ++index) {
        if (request.working_set->uses[index].key_index >=
            request.working_set->entries.size()) {
            return fail_submission(
                GlyphAtlasSubmissionErrorKind::TopologyViolation,
                0U,
                index,
                0U,
                0U,
                "glyph raster use references an invalid key",
                error);
        }
    }
    for (std::size_t index = 0U; index < request.raster_sources.size(); ++index) {
        if (!source_basic_valid(request.raster_sources[index], request.raster_payload) ||
            (index != 0U &&
             !key_less(
                 request.raster_sources[index - 1U].key,
                 request.raster_sources[index].key))) {
            return fail_submission(
                GlyphAtlasSubmissionErrorKind::InvalidRasterSource,
                0U,
                0U,
                index,
                0U,
                "glyph raster source table is invalid or not strictly sorted",
                error);
        }
    }

    std::scoped_lock lock(cache->mutex_);
    if (effective_stats != nullptr) {
        effective_stats->metadata_before =
            cache->ledger_.snapshot(core::ResourceClass::RasterTile);
        effective_stats->input_unique_keys = request.working_set->entries.size();
        effective_stats->input_uses = request.working_set->uses.size();
    }
    if (cache->use_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail_submission(
            GlyphAtlasSubmissionErrorKind::ArithmeticOverflow,
            0U,
            0U,
            0U,
            0U,
            "glyph atlas use epoch overflow",
            error);
    }
    const std::uint64_t epoch = cache->use_epoch_ + 1U;

    try {
        std::size_t potential_new_entries = 0U;
        for (const GlyphRasterWorkingSetEntry& working_entry :
             request.working_set->entries) {
            const auto iterator = find_entry(cache->entries_, working_entry.key);
            if (iterator == cache->entries_.end() ||
                !(iterator->key == working_entry.key)) {
                ++potential_new_entries;
            }
        }

        std::pmr::vector<GlyphAtlasPageRecord> staged_pages(
            &cache->metadata_resource_);
        std::pmr::vector<GlyphAtlasCacheEntry> staged_entries(
            &cache->metadata_resource_);
        std::pmr::vector<std::uint8_t> pinned_pages(&cache->metadata_resource_);
        staged_pages.reserve(config.maximum_pages);
        if (cache->pages_.empty()) {
            staged_pages.resize(config.maximum_pages);
        } else {
            if (cache->pages_.size() != config.maximum_pages) {
                return fail_submission(
                    GlyphAtlasSubmissionErrorKind::StaleCacheEntry,
                    0U,
                    0U,
                    0U,
                    0U,
                    "glyph atlas cache page topology is stale",
                    error);
            }
            staged_pages.insert(
                staged_pages.end(),
                cache->pages_.begin(),
                cache->pages_.end());
        }
        const std::size_t reserve_entries = std::min<std::size_t>(
            config.maximum_entries,
            cache->entries_.size() + potential_new_entries);
        staged_entries.reserve(reserve_entries);
        staged_entries.insert(
            staged_entries.end(),
            cache->entries_.begin(),
            cache->entries_.end());
        pinned_pages.resize(config.maximum_pages, 0U);

        std::size_t expected_uploads = 0U;
        for (const GlyphRasterWorkingSetEntry& working_entry :
             request.working_set->entries) {
            const auto cache_iterator = find_entry(
                staged_entries,
                working_entry.key);
            if (cache_iterator != staged_entries.end() &&
                cache_iterator->key == working_entry.key) {
                continue;
            }
            const auto source_iterator = find_source(
                request.raster_sources,
                working_entry.key);
            if (source_iterator == request.raster_sources.end() ||
                !(source_iterator->key == working_entry.key)) {
                return fail_submission(
                    GlyphAtlasSubmissionErrorKind::RasterSourceNotFound,
                    static_cast<std::size_t>(&working_entry -
                        request.working_set->entries.data()),
                    0U,
                    0U,
                    0U,
                    "raster source is required for a glyph atlas cache miss",
                    error);
            }
            if ((source_iterator->flags &
                 static_cast<std::uint8_t>(kGlyphRasterSourceEmpty)) == 0U) {
                ++expected_uploads;
            }
        }
        if (expected_uploads > request.limits.maximum_uploads) {
            return fail_submission(
                GlyphAtlasSubmissionErrorKind::SubmissionLimitExceeded,
                0U,
                0U,
                0U,
                0U,
                "glyph atlas upload count exceeds the submission limit",
                error);
        }
        std::pmr::vector<GlyphAtlasUploadRecord> staged_uploads(
            output->resource());
        staged_uploads.reserve(expected_uploads);

        std::uint64_t local_hits = 0U;
        std::uint64_t local_misses = 0U;
        std::uint64_t local_uploads = 0U;
        std::uint64_t local_upload_bytes = 0U;

        for (std::size_t key_index = 0U;
             key_index < request.working_set->entries.size();
             ++key_index) {
            const GlyphRasterWorkingSetEntry& working_entry =
                request.working_set->entries[key_index];
            auto cache_iterator = find_entry(&staged_entries, working_entry.key);
            const auto source_iterator = find_source(
                request.raster_sources,
                working_entry.key);
            const bool source_found =
                source_iterator != request.raster_sources.end() &&
                source_iterator->key == working_entry.key;
            if (cache_iterator != staged_entries.end() &&
                cache_iterator->key == working_entry.key) {
                ++local_hits;
                if (source_found &&
                    source_iterator->content_checksum !=
                        cache_iterator->raster_checksum) {
                    return fail_submission(
                        GlyphAtlasSubmissionErrorKind::RasterKeyCollision,
                        key_index,
                        0U,
                        static_cast<std::size_t>(
                            source_iterator - request.raster_sources.begin()),
                        cache_iterator->page_index,
                        "glyph raster key resolves to different content",
                        error);
                }
                cache_iterator->last_use_epoch = epoch;
                if ((cache_iterator->flags &
                     static_cast<std::uint8_t>(kGlyphAtlasCacheEntryEmpty)) == 0U) {
                    if (cache_iterator->page_index >= staged_pages.size()) {
                        return fail_submission(
                            GlyphAtlasSubmissionErrorKind::StaleCacheEntry,
                            key_index,
                            0U,
                            0U,
                            cache_iterator->page_index,
                            "glyph atlas cache entry references an invalid page",
                            error);
                    }
                    GlyphAtlasPageRecord& page =
                        staged_pages[cache_iterator->page_index];
                    if (page.initialized == 0U ||
                        page.generation != cache_iterator->page_generation ||
                        page.format != cache_iterator->format) {
                        return fail_submission(
                            GlyphAtlasSubmissionErrorKind::StaleCacheEntry,
                            key_index,
                            0U,
                            0U,
                            cache_iterator->page_index,
                            "glyph atlas cache entry generation is stale",
                            error);
                    }
                    page.last_use_epoch = epoch;
                    pinned_pages[cache_iterator->page_index] = 1U;
                }
                continue;
            }

            ++local_misses;
            if (!source_found) {
                return fail_submission(
                    GlyphAtlasSubmissionErrorKind::RasterSourceNotFound,
                    key_index,
                    0U,
                    0U,
                    0U,
                    "raster source is required for a glyph atlas cache miss",
                    error);
            }
            const std::size_t source_index = static_cast<std::size_t>(
                source_iterator - request.raster_sources.begin());
            const GlyphRasterSourceRecord& source = *source_iterator;
            const bool empty =
                (source.flags &
                 static_cast<std::uint8_t>(kGlyphRasterSourceEmpty)) != 0U;
            if (!empty) {
                const std::span<const std::byte> bytes = request.raster_payload.subspan(
                    static_cast<std::size_t>(source.payload_offset),
                    static_cast<std::size_t>(source.payload_size));
                if (fnv1a64(bytes) != source.content_checksum) {
                    return fail_submission(
                        GlyphAtlasSubmissionErrorKind::RasterChecksumMismatch,
                        key_index,
                        0U,
                        source_index,
                        0U,
                        "glyph raster payload checksum mismatch",
                        error);
                }
                if (source.width > config.page_width ||
                    source.height > config.page_height) {
                    return fail_submission(
                        GlyphAtlasSubmissionErrorKind::AtlasCapacityExceeded,
                        key_index,
                        0U,
                        source_index,
                        0U,
                        "glyph raster exceeds atlas page dimensions",
                        error);
                }
            }
            if (!ensure_entry_capacity(
                    config,
                    &staged_pages,
                    &staged_entries,
                    pinned_pages,
                    epoch,
                    effective_stats,
                    error,
                    key_index)) {
                return false;
            }

            GlyphAtlasCacheEntry new_entry;
            new_entry.key = working_entry.key;
            new_entry.last_use_epoch = epoch;
            new_entry.raster_checksum = source.content_checksum;
            new_entry.width = source.width;
            new_entry.height = source.height;
            new_entry.bearing_x = source.bearing_x;
            new_entry.bearing_y = source.bearing_y;
            new_entry.format = source.format;
            if (empty) {
                new_entry.flags = kGlyphAtlasCacheEntryEmpty;
            } else {
                std::uint32_t page_index = 0U;
                Placement placement;
                if (!acquire_page_for_source(
                        source,
                        config,
                        &staged_pages,
                        &staged_entries,
                        &pinned_pages,
                        epoch,
                        &page_index,
                        &placement,
                        effective_stats,
                        error,
                        key_index)) {
                    return false;
                }
                GlyphAtlasPageRecord& page = staged_pages[page_index];
                const std::uint64_t glyph_area =
                    static_cast<std::uint64_t>(source.width) * source.height;
                if (glyph_area >
                    std::numeric_limits<std::uint32_t>::max() - page.used_area) {
                    return fail_submission(
                        GlyphAtlasSubmissionErrorKind::ArithmeticOverflow,
                        key_index,
                        0U,
                        source_index,
                        page_index,
                        "glyph atlas used-area overflow",
                        error);
                }
                page.next_x = placement.next_x;
                page.next_y = placement.next_y;
                page.row_height = placement.row_height;
                page.used_area += static_cast<std::uint32_t>(glyph_area);
                ++page.live_entries;
                page.last_use_epoch = epoch;
                pinned_pages[page_index] = 1U;
                new_entry.page_index = page_index;
                new_entry.page_generation = page.generation;
                new_entry.atlas_x = placement.x;
                new_entry.atlas_y = placement.y;

                if (staged_uploads.size() >= request.limits.maximum_uploads ||
                    !add_unsigned(
                        local_upload_bytes,
                        source.payload_size,
                        &local_upload_bytes) ||
                    local_upload_bytes > request.limits.maximum_upload_bytes) {
                    return fail_submission(
                        GlyphAtlasSubmissionErrorKind::SubmissionLimitExceeded,
                        key_index,
                        0U,
                        source_index,
                        page_index,
                        "glyph atlas upload limit exceeded",
                        error);
                }
                GlyphAtlasUploadRecord upload;
                upload.atlas_generation_id = cache->atlas_generation_id_;
                upload.page_generation = page.generation;
                upload.payload_offset = source.payload_offset;
                upload.payload_size = source.payload_size;
                upload.page_index = page_index;
                upload.atlas_x = placement.x;
                upload.atlas_y = placement.y;
                upload.width = source.width;
                upload.height = source.height;
                upload.row_bytes = source.row_bytes;
                upload.working_set_key_index =
                    static_cast<std::uint32_t>(key_index);
                upload.format = source.format;
                staged_uploads.push_back(upload);
                ++local_uploads;
            }
            cache_iterator = find_entry(&staged_entries, new_entry.key);
            staged_entries.insert(cache_iterator, new_entry);
        }

        std::size_t expected_instances = 0U;
        std::size_t expected_batches = 0U;
        std::uint32_t previous_page_index = 0U;
        std::uint64_t previous_page_generation = 0U;
        std::uint32_t previous_style_id = 0U;
        std::uint32_t previous_clip_index = 0U;
        bool have_previous = false;
        for (const GlyphRasterUseRecord& use : request.working_set->uses) {
            const GlyphRasterKey& key =
                request.working_set->entries[use.key_index].key;
            const auto cache_iterator = find_entry(staged_entries, key);
            if (cache_iterator == staged_entries.end() ||
                !(cache_iterator->key == key)) {
                return fail_submission(
                    GlyphAtlasSubmissionErrorKind::StaleCacheEntry,
                    use.key_index,
                    0U,
                    0U,
                    0U,
                    "glyph atlas entry disappeared before output sizing",
                    error);
            }
            if ((cache_iterator->flags &
                 static_cast<std::uint8_t>(kGlyphAtlasCacheEntryEmpty)) != 0U) {
                continue;
            }
            ++expected_instances;
            const bool same_batch = have_previous &&
                previous_page_generation == cache_iterator->page_generation &&
                previous_page_index == cache_iterator->page_index &&
                previous_style_id == use.style_id &&
                previous_clip_index == use.clip_index;
            if (!same_batch) {
                ++expected_batches;
            }
            have_previous = true;
            previous_page_generation = cache_iterator->page_generation;
            previous_page_index = cache_iterator->page_index;
            previous_style_id = use.style_id;
            previous_clip_index = use.clip_index;
        }
        if (expected_instances > request.limits.maximum_draw_instances ||
            expected_batches > request.limits.maximum_draw_batches ||
            expected_instances > std::numeric_limits<std::uint32_t>::max()) {
            return fail_submission(
                GlyphAtlasSubmissionErrorKind::SubmissionLimitExceeded,
                0U,
                0U,
                0U,
                0U,
                "glyph atlas draw output exceeds compact submission limits",
                error);
        }
        std::pmr::vector<GlyphAtlasDrawInstance> staged_instances(
            output->resource());
        std::pmr::vector<GlyphAtlasDrawBatch> staged_batches(
            output->resource());
        staged_instances.reserve(expected_instances);
        staged_batches.reserve(expected_batches);

        for (std::size_t use_index = 0U;
             use_index < request.working_set->uses.size();
             ++use_index) {
            const GlyphRasterUseRecord& use = request.working_set->uses[use_index];
            const GlyphRasterKey& key =
                request.working_set->entries[use.key_index].key;
            const auto cache_iterator = find_entry(staged_entries, key);
            if (cache_iterator == staged_entries.end() ||
                !(cache_iterator->key == key)) {
                return fail_submission(
                    GlyphAtlasSubmissionErrorKind::StaleCacheEntry,
                    use.key_index,
                    use_index,
                    0U,
                    0U,
                    "glyph atlas entry disappeared before draw submission",
                    error);
            }
            if ((cache_iterator->flags &
                 static_cast<std::uint8_t>(kGlyphAtlasCacheEntryEmpty)) != 0U) {
                if (effective_stats != nullptr) {
                    ++effective_stats->empty_glyphs;
                }
                continue;
            }
            if (staged_instances.size() >= request.limits.maximum_draw_instances) {
                return fail_submission(
                    GlyphAtlasSubmissionErrorKind::SubmissionLimitExceeded,
                    use.key_index,
                    use_index,
                    0U,
                    cache_iterator->page_index,
                    "glyph atlas draw-instance limit exceeded",
                    error);
            }
            std::int64_t inline_start = 0;
            std::int64_t block_start = 0;
            if (!add_signed(
                    use.viewport_inline_origin,
                    cache_iterator->bearing_x,
                    &inline_start) ||
                !subtract_signed(
                    use.viewport_baseline_origin,
                    cache_iterator->bearing_y,
                    &block_start)) {
                return fail_submission(
                    GlyphAtlasSubmissionErrorKind::ArithmeticOverflow,
                    use.key_index,
                    use_index,
                    0U,
                    cache_iterator->page_index,
                    "glyph atlas draw coordinate overflow",
                    error);
            }
            GlyphAtlasDrawInstance instance;
            instance.viewport_inline_start = inline_start;
            instance.viewport_block_start = block_start;
            instance.atlas_generation_id = cache->atlas_generation_id_;
            instance.page_generation = cache_iterator->page_generation;
            instance.page_index = cache_iterator->page_index;
            instance.atlas_x = cache_iterator->atlas_x;
            instance.atlas_y = cache_iterator->atlas_y;
            instance.width = cache_iterator->width;
            instance.height = cache_iterator->height;
            instance.style_id = use.style_id;
            instance.clip_index = use.clip_index;
            instance.working_set_key_index = use.key_index;
            if (staged_instances.size() >
                std::numeric_limits<std::uint32_t>::max()) {
                return fail_submission(
                    GlyphAtlasSubmissionErrorKind::AggregateOverflow,
                    use.key_index,
                    use_index,
                    0U,
                    instance.page_index,
                    "glyph atlas instance index exceeds compact range",
                    error);
            }
            const std::uint32_t instance_index =
                static_cast<std::uint32_t>(staged_instances.size());
            staged_instances.push_back(instance);

            const bool can_coalesce = !staged_batches.empty() &&
                staged_batches.back().page_generation == instance.page_generation &&
                staged_batches.back().page_index == instance.page_index &&
                staged_batches.back().style_id == instance.style_id &&
                staged_batches.back().clip_index == instance.clip_index &&
                static_cast<std::uint64_t>(staged_batches.back().first_instance) +
                        staged_batches.back().instance_count ==
                    instance_index;
            if (can_coalesce) {
                if (staged_batches.back().instance_count ==
                    std::numeric_limits<std::uint32_t>::max()) {
                    return fail_submission(
                        GlyphAtlasSubmissionErrorKind::AggregateOverflow,
                        use.key_index,
                        use_index,
                        0U,
                        instance.page_index,
                        "glyph atlas draw batch count overflow",
                        error);
                }
                ++staged_batches.back().instance_count;
                staged_batches.back().flags |= kGlyphAtlasDrawBatchCoalesced;
                if (effective_stats != nullptr) {
                    ++effective_stats->coalesced_instances;
                }
            } else {
                if (staged_batches.size() >= request.limits.maximum_draw_batches) {
                    return fail_submission(
                        GlyphAtlasSubmissionErrorKind::SubmissionLimitExceeded,
                        use.key_index,
                        use_index,
                        0U,
                        instance.page_index,
                        "glyph atlas draw-batch limit exceeded",
                        error);
                }
                GlyphAtlasDrawBatch batch;
                batch.page_generation = instance.page_generation;
                batch.page_index = instance.page_index;
                batch.style_id = instance.style_id;
                batch.clip_index = instance.clip_index;
                batch.first_instance = instance_index;
                batch.instance_count = 1U;
                staged_batches.push_back(batch);
            }
        }

        if (effective_stats != nullptr) {
            effective_stats->cache_hits = local_hits;
            effective_stats->cache_misses = local_misses;
            effective_stats->uploads = local_uploads;
            effective_stats->upload_bytes = local_upload_bytes;
            effective_stats->draw_instances = staged_instances.size();
            effective_stats->draw_batches = staged_batches.size();
            for (const GlyphAtlasPageRecord& page : staged_pages) {
                effective_stats->maximum_page_live_entries = std::max<std::uint64_t>(
                    effective_stats->maximum_page_live_entries,
                    page.live_entries);
            }
            for (const GlyphAtlasDrawBatch& batch : staged_batches) {
                effective_stats->maximum_instances_per_batch = std::max<std::uint64_t>(
                    effective_stats->maximum_instances_per_batch,
                    batch.instance_count);
            }
        }

        cache->pages_.swap(staged_pages);
        cache->entries_.swap(staged_entries);
        cache->use_epoch_ = epoch;
        cache->hits_ += local_hits;
        cache->misses_ += local_misses;
        cache->uploads_ += local_uploads;
        if (effective_stats != nullptr) {
            cache->evicted_entries_ += effective_stats->evicted_entries;
            cache->reset_pages_ += effective_stats->reset_pages;
        }
        for (std::uint64_t index = 0U; index < local_hits; ++index) {
            cache->ledger_.record_cache_hit(core::ResourceClass::RasterTile);
        }
        for (std::uint64_t index = 0U; index < local_misses; ++index) {
            cache->ledger_.record_cache_miss(core::ResourceClass::RasterTile);
        }
        if (effective_stats != nullptr) {
            for (std::uint64_t index = 0U; index < effective_stats->evicted_entries; ++index) {
                cache->ledger_.record_eviction(core::ResourceClass::RasterTile);
            }
        }

        output->atlas_generation_id = cache->atlas_generation_id_;
        output->submission_epoch = epoch;
        output->uploads.swap(staged_uploads);
        output->draw_instances.swap(staged_instances);
        output->draw_batches.swap(staged_batches);

        release_vector(&staged_pages);
        release_vector(&staged_entries);
        release_vector(&pinned_pages);
        if (effective_stats != nullptr) {
            effective_stats->metadata_after =
                cache->ledger_.snapshot(core::ResourceClass::RasterTile);
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail_submission(
            GlyphAtlasSubmissionErrorKind::MetadataBudgetExceeded,
            0U,
            0U,
            0U,
            0U,
            "glyph atlas cache metadata or submission output budget exceeded",
            error);
    } catch (...) {
        return fail_submission(
            GlyphAtlasSubmissionErrorKind::InvalidInput,
            0U,
            0U,
            0U,
            0U,
            "unexpected glyph atlas submission failure",
            error);
    }
}

bool glyph_atlas_submission_is_current(
    const GlyphAtlasCache& cache,
    const GlyphAtlasSubmission& submission) noexcept {
    std::scoped_lock lock(cache.mutex_);
    if (submission.atlas_generation_id == 0U ||
        submission.atlas_generation_id != cache.atlas_generation_id_) {
        return false;
    }
    const auto current = [&cache](
                             std::uint32_t page_index,
                             std::uint64_t page_generation) noexcept {
        return page_index < cache.pages_.size() &&
            cache.pages_[page_index].initialized != 0U &&
            cache.pages_[page_index].generation == page_generation;
    };
    for (const GlyphAtlasUploadRecord& upload : submission.uploads) {
        if (upload.atlas_generation_id != submission.atlas_generation_id ||
            !current(upload.page_index, upload.page_generation)) {
            return false;
        }
    }
    for (const GlyphAtlasDrawInstance& instance : submission.draw_instances) {
        if (instance.atlas_generation_id != submission.atlas_generation_id ||
            !current(instance.page_index, instance.page_generation)) {
            return false;
        }
    }
    for (const GlyphAtlasDrawBatch& batch : submission.draw_batches) {
        if (!current(batch.page_index, batch.page_generation) ||
            batch.first_instance > submission.draw_instances.size() ||
            batch.instance_count >
                submission.draw_instances.size() - batch.first_instance) {
            return false;
        }
    }
    return true;
}

} // namespace zevryon::text
