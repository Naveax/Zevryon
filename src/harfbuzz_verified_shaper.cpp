#include "harfbuzz_shaper.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
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

bool fail_input(
    std::size_t input_index,
    const char* message,
    HarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        error->kind = HarfBuzzShapingErrorKind::InvalidInput;
        error->input_index = input_index;
        error->sfnt_parse_error = SfntParseErrorKind::None;
        error->sfnt_integrity_error = SfntIntegrityErrorKind::None;
        error->font_table_tag = 0U;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool fail_resource(
    const char* message,
    HarfBuzzShapingError* error) noexcept {
    if (error != nullptr) {
        error->kind = HarfBuzzShapingErrorKind::InvalidFontData;
        error->input_index = 0U;
        error->sfnt_parse_error = SfntParseErrorKind::None;
        error->sfnt_integrity_error = SfntIntegrityErrorKind::InvalidView;
        error->font_table_tag = 0U;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
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
    std::uint64_t resource_id,
    bool used_resource,
    bool inline_verification,
    HarfBuzzShapingStats* stats) noexcept {
    stats->validated_font_faces = parse_stats.face_count;
    stats->validated_font_tables = parse_stats.table_count;
    stats->verified_font_table_checksums = integrity_stats.checksums_verified;
    stats->verified_font_payload_bytes = integrity_stats.payload_bytes_checked;
    stats->verified_font_padding_bytes = integrity_stats.padding_bytes_checked;
    stats->verified_font_resource_id = resource_id;
    stats->whole_font_checksum_verified =
        integrity_stats.whole_font_checksum_verified;
    stats->whole_font_checksum_ignored_for_collection =
        integrity_stats.whole_font_checksum_ignored_for_collection;
    stats->used_verified_font_resource = used_resource;
    stats->performed_inline_font_verification = inline_verification;
}

bool copy_backend_failure(
    HarfBuzzShapingError* destination,
    HarfBuzzShapingError* source) noexcept {
    destination->kind = source->kind;
    destination->input_index = source->input_index;
    destination->sfnt_parse_error = source->sfnt_parse_error;
    destination->sfnt_integrity_error = source->sfnt_integrity_error;
    destination->font_table_tag = source->font_table_tag;
    destination->message.swap(source->message);
    return false;
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

    HarfBuzzShapingRequest backend_request = request;
    SfntParseStats parse_stats;
    SfntIntegrityStats integrity_stats;
    std::uint64_t resource_id = 0U;
    bool used_resource = false;
    bool inline_verification = false;

    const std::shared_ptr<const VerifiedFontResource> retained =
        request.verified_font_resource;
    if (retained != nullptr) {
        if (!request.font_bytes.empty()) {
            return fail_input(
                0U,
                "raw font_bytes and verified_font_resource are mutually exclusive",
                error);
        }
        if (!retained->view().valid() || retained->bytes().empty() ||
            retained->bytes().data() != retained->view().bytes().data() ||
            !retained->accounting_clean() || !retained->within_hard_limit()) {
            return fail_resource(
                "verified font resource ownership or accounting invariant failed",
                error);
        }
        if (request.face_index != retained->view().face_index()) {
            return fail_input(
                request.face_index,
                "request face_index does not match verified font resource",
                error);
        }

        backend_request.font_bytes = retained->bytes();
        backend_request.verified_font_resource.reset();
        parse_stats = retained->parse_stats();
        integrity_stats = retained->integrity_stats();
        resource_id = retained->resource_id();
        used_resource = true;
    } else {
        SfntResourceView view;
        SfntParseError parse_error;
        if (!open_sfnt_resource(
                request.font_bytes,
                request.face_index,
                &view,
                &parse_stats,
                &parse_error)) {
            return fail_font_parse(parse_error, error);
        }

        SfntIntegrityError integrity_error;
        if (!verify_sfnt_integrity(
                view,
                SfntIntegrityOptions{},
                &integrity_stats,
                &integrity_error)) {
            return fail_font_integrity(integrity_error, error);
        }
        inline_verification = true;
    }

    HarfBuzzShapingStats backend_stats;
    HarfBuzzShapingError backend_error;
    if (!shape_harfbuzz_segment_backend(
            backend_request,
            output,
            &backend_stats,
            &backend_error)) {
        return copy_backend_failure(error, &backend_error);
    }

    publish_validation_stats(
        parse_stats,
        integrity_stats,
        resource_id,
        used_resource,
        inline_verification,
        &backend_stats);
    *stats = backend_stats;
    return true;
}

} // namespace zevryon::text
