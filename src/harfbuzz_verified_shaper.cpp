#include "harfbuzz_shaper.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace zevryon::text {

// Private implementation symbol produced from harfbuzz_shaper_backend.cpp.
bool shape_harfbuzz_segment_backend(
    const HarfBuzzShapingRequest& request,
    ShapedGlyphRun* output,
    HarfBuzzShapingStats* stats,
    HarfBuzzShapingError* error) noexcept;

namespace {

void clear_error(HarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        error->kind = HarfBuzzShapingErrorKind::None;
        error->input_index = 0U;
        error->sfnt_parse_error = SfntParseErrorKind::None;
        error->sfnt_integrity_error = SfntIntegrityErrorKind::None;
        error->font_table_tag = 0U;
        error->message.clear();
    }
}

bool fail_font_parse(
    const SfntParseError& source,
    HarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        error->kind = HarfBuzzShapingErrorKind::InvalidFontData;
        error->input_index = source.byte_offset;
        error->sfnt_parse_error = source.kind;
        error->sfnt_integrity_error = SfntIntegrityErrorKind::None;
        error->font_table_tag = 0U;
        try {
            error->message = "font failed SFNT/TTC structural validation: ";
            error->message += sfnt_parse_error_kind_name(source.kind);
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool fail_font_integrity(
    const SfntIntegrityError& source,
    HarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        error->kind = HarfBuzzShapingErrorKind::InvalidFontData;
        error->input_index = source.byte_offset;
        error->sfnt_parse_error = SfntParseErrorKind::None;
        error->sfnt_integrity_error = source.kind;
        error->font_table_tag = source.table_tag;
        try {
            error->message = "font failed SFNT integrity validation: ";
            error->message += sfnt_integrity_error_kind_name(source.kind);
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

void publish_validation_stats(
    const SfntParseStats& parse_stats,
    const SfntIntegrityStats& integrity_stats,
    HarfBuzzShapingStats* stats) noexcept {
    stats->validated_font_faces = parse_stats.face_count;
    stats->validated_font_tables = parse_stats.table_count;
    stats->verified_font_table_checksums = integrity_stats.checksums_verified;
    stats->verified_font_payload_bytes = integrity_stats.payload_bytes_checked;
    stats->verified_font_padding_bytes = integrity_stats.padding_bytes_checked;
    stats->whole_font_checksum_verified =
        integrity_stats.whole_font_checksum_verified;
    stats->whole_font_checksum_ignored_for_collection =
        integrity_stats.whole_font_checksum_ignored_for_collection;
}

} // namespace

bool shape_harfbuzz_segment(
    const HarfBuzzShapingRequest& request,
    ShapedGlyphRun* output,
    HarfBuzzShapingStats* stats,
    HarfBuzzShapingError* error) noexcept {
    if (output == nullptr || stats == nullptr || error == nullptr) {
        return false;
    }
    output->release();
    *stats = {};
    clear_error(error);

    SfntResourceView view;
    SfntParseStats parse_stats;
    SfntParseError parse_error;
    if (!open_sfnt_resource(
            request.font_bytes,
            request.face_index,
            &view,
            &parse_stats,
            &parse_error)) {
        return fail_font_parse(parse_error, error);
    }

    SfntIntegrityStats integrity_stats;
    SfntIntegrityError integrity_error;
    if (!verify_sfnt_integrity(
            view,
            SfntIntegrityOptions{},
            &integrity_stats,
            &integrity_error)) {
        return fail_font_integrity(integrity_error, error);
    }

    HarfBuzzShapingStats backend_stats;
    HarfBuzzShapingError backend_error;
    if (!shape_harfbuzz_segment_backend(
            request,
            output,
            &backend_stats,
            &backend_error)) {
        error->kind = backend_error.kind;
        error->input_index = backend_error.input_index;
        error->sfnt_parse_error = backend_error.sfnt_parse_error;
        error->sfnt_integrity_error = backend_error.sfnt_integrity_error;
        error->font_table_tag = backend_error.font_table_tag;
        error->message.swap(backend_error.message);
        return false;
    }

    publish_validation_stats(parse_stats, integrity_stats, &backend_stats);
    *stats = backend_stats;
    return true;
}

} // namespace zevryon::text
