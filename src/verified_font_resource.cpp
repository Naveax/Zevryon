#include "verified_font_resource.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace zevryon::text {
namespace {

void clear_error(VerifiedFontResourceError* error) noexcept {
    if (error != nullptr) {
        error->kind = VerifiedFontResourceErrorKind::None;
        error->byte_offset = 0U;
        error->table_tag = 0U;
        error->parse_error = SfntParseErrorKind::None;
        error->integrity_error = SfntIntegrityErrorKind::None;
        error->message.clear();
    }
}

bool fail(
    VerifiedFontResourceErrorKind kind,
    std::size_t byte_offset,
    std::uint32_t table_tag,
    SfntParseErrorKind parse_error,
    SfntIntegrityErrorKind integrity_error,
    const char* message,
    VerifiedFontResourceError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->byte_offset = byte_offset;
        error->table_tag = table_tag;
        error->parse_error = parse_error;
        error->integrity_error = integrity_error;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

} // namespace

VerifiedFontResource::VerifiedFontResource(
    std::uint64_t resource_id,
    std::size_t hard_limit) noexcept
    : byte_resource_(ledger_, core::ResourceClass::FontResourceBytes),
      bytes_(&byte_resource_),
      resource_id_(resource_id) {
    ledger_.set_hard_limit(core::ResourceClass::FontResourceBytes, hard_limit);
}

std::uint64_t VerifiedFontResource::resource_id() const noexcept {
    return resource_id_;
}

std::span<const std::byte> VerifiedFontResource::bytes() const noexcept {
    return bytes_;
}

const SfntResourceView& VerifiedFontResource::view() const noexcept {
    return view_;
}

const SfntParseStats& VerifiedFontResource::parse_stats() const noexcept {
    return parse_stats_;
}

const SfntIntegrityStats& VerifiedFontResource::integrity_stats() const noexcept {
    return integrity_stats_;
}

core::ResourceSnapshot VerifiedFontResource::resource_snapshot() const noexcept {
    return ledger_.snapshot(core::ResourceClass::FontResourceBytes);
}

bool VerifiedFontResource::accounting_clean() const noexcept {
    return ledger_.accounting_clean();
}

bool VerifiedFontResource::within_hard_limit() const noexcept {
    return ledger_.within_hard_limits();
}

const char* verified_font_resource_error_kind_name(
    VerifiedFontResourceErrorKind kind) noexcept {
    switch (kind) {
    case VerifiedFontResourceErrorKind::None:
        return "none";
    case VerifiedFontResourceErrorKind::InvalidArgument:
        return "invalid_argument";
    case VerifiedFontResourceErrorKind::OutputBudgetExceeded:
        return "output_budget_exceeded";
    case VerifiedFontResourceErrorKind::AllocationFailed:
        return "allocation_failed";
    case VerifiedFontResourceErrorKind::ParseFailed:
        return "parse_failed";
    case VerifiedFontResourceErrorKind::IntegrityFailed:
        return "integrity_failed";
    }
    return "unknown";
}

bool build_verified_font_resource(
    std::uint64_t resource_id,
    std::span<const std::byte> source,
    std::uint32_t face_index,
    std::size_t hard_limit,
    std::shared_ptr<const VerifiedFontResource>* output,
    VerifiedFontResourceStats* stats,
    VerifiedFontResourceError* error) noexcept {
    if (output != nullptr) {
        output->reset();
    }
    if (stats != nullptr) {
        *stats = {};
    }
    clear_error(error);

    if (output == nullptr || resource_id == 0U) {
        return fail(
            VerifiedFontResourceErrorKind::InvalidArgument,
            0U,
            0U,
            SfntParseErrorKind::None,
            SfntIntegrityErrorKind::None,
            "output is required and resource_id must be non-zero",
            error);
    }
    if (source.size() > hard_limit) {
        return fail(
            VerifiedFontResourceErrorKind::OutputBudgetExceeded,
            source.size(),
            0U,
            SfntParseErrorKind::None,
            SfntIntegrityErrorKind::None,
            "font bytes exceed the retained resource hard limit",
            error);
    }

    std::shared_ptr<VerifiedFontResource> candidate;
    try {
        candidate.reset(new VerifiedFontResource(resource_id, hard_limit));
        candidate->bytes_.reserve(source.size());
        candidate->bytes_.insert(
            candidate->bytes_.end(), source.begin(), source.end());
    } catch (const std::bad_alloc&) {
        if (candidate != nullptr &&
            candidate->resource_snapshot().rejected_reservations != 0U) {
            return fail(
                VerifiedFontResourceErrorKind::OutputBudgetExceeded,
                source.size(),
                0U,
                SfntParseErrorKind::None,
                SfntIntegrityErrorKind::None,
                "font byte retention exceeded the resource ledger limit",
                error);
        }
        return fail(
            VerifiedFontResourceErrorKind::AllocationFailed,
            source.size(),
            0U,
            SfntParseErrorKind::None,
            SfntIntegrityErrorKind::None,
            "font resource allocation failed",
            error);
    } catch (...) {
        return fail(
            VerifiedFontResourceErrorKind::AllocationFailed,
            source.size(),
            0U,
            SfntParseErrorKind::None,
            SfntIntegrityErrorKind::None,
            "unexpected font resource allocation failure",
            error);
    }

    SfntResourceView view;
    SfntParseStats parse_stats;
    SfntParseError parse_error;
    if (!open_sfnt_resource(
            candidate->bytes_,
            face_index,
            &view,
            &parse_stats,
            &parse_error)) {
        return fail(
            VerifiedFontResourceErrorKind::ParseFailed,
            parse_error.byte_offset,
            0U,
            parse_error.kind,
            SfntIntegrityErrorKind::None,
            "retained font failed SFNT/TTC structural validation",
            error);
    }

    SfntIntegrityStats integrity_stats;
    SfntIntegrityError integrity_error;
    if (!verify_sfnt_integrity(
            view,
            SfntIntegrityOptions{},
            &integrity_stats,
            &integrity_error)) {
        return fail(
            VerifiedFontResourceErrorKind::IntegrityFailed,
            integrity_error.byte_offset,
            integrity_error.table_tag,
            SfntParseErrorKind::None,
            integrity_error.kind,
            "retained font failed strict SFNT integrity verification",
            error);
    }

    candidate->view_ = view;
    candidate->parse_stats_ = parse_stats;
    candidate->integrity_stats_ = integrity_stats;

    if (stats != nullptr) {
        stats->resource_id = resource_id;
        stats->source_bytes = source.size();
        stats->retained_bytes = candidate->bytes_.size();
        stats->parse = parse_stats;
        stats->integrity = integrity_stats;
    }

    *output = std::move(candidate);
    return true;
}

} // namespace zevryon::text
