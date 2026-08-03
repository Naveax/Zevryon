#include "resource_ledger.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
#ifndef ZEVRYON_RUST_LEDGER_SHADOW_INTERVAL
#define ZEVRYON_RUST_LEDGER_SHADOW_INTERVAL 1024
#endif
#ifndef ZEVRYON_RUST_LEDGER_SHADOW_STRICT
#define ZEVRYON_RUST_LEDGER_SHADOW_STRICT 0
#endif
#endif

namespace zevryon::core {
namespace {

constexpr std::array<const char*, resource_class_count> kResourceNames{
    "source_window",
    "checkpoint_index",
    "unicode_buffer",
    "grapheme_cluster",
    "script_run",
    "bidi_run",
    "bidi_sequence",
    "bidi_type_resolution",
    "bidi_neutral_resolution",
    "glyph_run",
    "computed_style",
    "layout_fragment",
    "paint_command",
    "raster_tile",
    "image_decode",
    "javascript_heap",
    "accessibility_projection",
    "network_buffer",
    "dom_projection",
    "compositor_surface",
    "bidi_implicit_level",
    "bidi_visual_order",
    "bidi_mirror_request",
    "font_catalog",
    "font_fallback_plan",
    "font_discovery_snapshot",
    "font_resource_bytes",
    "font_resource_cache_metadata",
    "font_resource_cache_retention",
    "font_file_read_buffer",
    "shaping_run_plan",
    "multi_run_shape_metadata",
    "glyph_cluster_map",
    "caret_boundary_map",
    "line_break_opportunity_map",
    "line_selection_map",
};

} // namespace

const char* resource_class_name(ResourceClass resource_class) noexcept {
    const std::size_t index = static_cast<std::size_t>(resource_class);
    return index < kResourceNames.size() ? kResourceNames[index] : "invalid";
}

ResourceLedger::ResourceLedger() noexcept {
    for (ResourceSnapshot& resource : resources_) {
        resource.hard_limit_bytes = std::numeric_limits<std::size_t>::max();
    }

#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    rust_shadow_initialized_ = zr_ledger_init(&rust_shadow_storage_) != 0U;
    if (!rust_shadow_initialized_) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::RustUnavailable,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            1U,
            0U);
        return;
    }

    if (zr_abi_version() != ZR_ABI_VERSION) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::AbiVersion,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            ZR_ABI_VERSION,
            zr_abi_version());
        rust_shadow_initialized_ = false;
        return;
    }

    if (zr_resource_class_count() != ZR_RESOURCE_CLASS_COUNT) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ResourceClassCount,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            ZR_RESOURCE_CLASS_COUNT,
            zr_resource_class_count());
        rust_shadow_initialized_ = false;
    }
#endif
}

void ResourceLedger::set_hard_limit(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    resources_[index_of(resource_class)].hard_limit_bytes = bytes;
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    rust_shadow_set_hard_limit(resource_class, bytes);
#endif
}

bool ResourceLedger::try_reserve(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    ResourceSnapshot& resource = resources_[index_of(resource_class)];
    bool reserved = true;
    if (bytes > resource.hard_limit_bytes ||
        resource.current_bytes > resource.hard_limit_bytes - bytes ||
        bytes > std::numeric_limits<std::size_t>::max() - total_current_bytes_) {
        ++resource.rejected_reservations;
        reserved = false;
    } else {
        resource.current_bytes += bytes;
        resource.peak_bytes = std::max(resource.peak_bytes, resource.current_bytes);
        ++resource.reservations;
        total_current_bytes_ += bytes;
        total_peak_bytes_ = std::max(total_peak_bytes_, total_current_bytes_);
    }

#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    rust_shadow_try_reserve(resource_class, bytes, reserved);
#endif
    return reserved;
}

void ResourceLedger::release(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    ResourceSnapshot& resource = resources_[index_of(resource_class)];
    ++resource.releases;
    if (bytes > resource.current_bytes || bytes > total_current_bytes_) {
        ++resource.accounting_errors;
        total_current_bytes_ -= std::min(total_current_bytes_, resource.current_bytes);
        resource.current_bytes = 0U;
    } else {
        resource.current_bytes -= bytes;
        total_current_bytes_ -= bytes;
    }

#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    rust_shadow_release(resource_class, bytes);
#endif
}

void ResourceLedger::record_cache_hit(ResourceClass resource_class) noexcept {
    ++resources_[index_of(resource_class)].cache_hits;
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    rust_shadow_record_cache_hit(resource_class);
#endif
}

void ResourceLedger::record_cache_miss(ResourceClass resource_class) noexcept {
    ++resources_[index_of(resource_class)].cache_misses;
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    rust_shadow_record_cache_miss(resource_class);
#endif
}

void ResourceLedger::record_eviction(ResourceClass resource_class) noexcept {
    ++resources_[index_of(resource_class)].evictions;
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    rust_shadow_record_eviction(resource_class);
#endif
}

void ResourceLedger::record_physical_read(
    ResourceClass resource_class,
    std::uint64_t bytes) noexcept {
    ResourceSnapshot& resource = resources_[index_of(resource_class)];
    resource.physical_read_bytes = saturating_add(resource.physical_read_bytes, bytes);
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    rust_shadow_record_physical_read(resource_class, bytes);
#endif
}

void ResourceLedger::record_physical_write(
    ResourceClass resource_class,
    std::uint64_t bytes) noexcept {
    ResourceSnapshot& resource = resources_[index_of(resource_class)];
    resource.physical_write_bytes = saturating_add(resource.physical_write_bytes, bytes);
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    rust_shadow_record_physical_write(resource_class, bytes);
#endif
}

ResourceSnapshot ResourceLedger::snapshot(ResourceClass resource_class) const noexcept {
    return resources_[index_of(resource_class)];
}

std::size_t ResourceLedger::total_current_bytes() const noexcept {
    return total_current_bytes_;
}

std::size_t ResourceLedger::total_peak_bytes() const noexcept {
    return total_peak_bytes_;
}

bool ResourceLedger::within_hard_limits() const noexcept {
    for (const ResourceSnapshot& resource : resources_) {
        if (resource.current_bytes > resource.hard_limit_bytes ||
            resource.peak_bytes > resource.hard_limit_bytes) {
            return false;
        }
    }
    return true;
}

bool ResourceLedger::accounting_clean() const noexcept {
    for (const ResourceSnapshot& resource : resources_) {
        if (resource.accounting_errors != 0U) {
            return false;
        }
    }
    return true;
}

std::string ResourceLedger::json() const {
    std::ostringstream output;
    output << "{\"schema\":\"zevryon.resource-ledger.v1\","
           << "\"total_current_bytes\":" << total_current_bytes_ << ','
           << "\"total_peak_bytes\":" << total_peak_bytes_ << ','
           << "\"within_hard_limits\":"
           << (within_hard_limits() ? "true" : "false") << ','
           << "\"accounting_clean\":"
           << (accounting_clean() ? "true" : "false") << ','
           << "\"resources\":{";

    for (std::size_t index = 0U; index < resources_.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        const ResourceSnapshot& resource = resources_[index];
        output << '\"' << kResourceNames[index] << "\":{";
        output << "\"hard_limit_bytes\":" << resource.hard_limit_bytes << ',';
        output << "\"current_bytes\":" << resource.current_bytes << ',';
        output << "\"peak_bytes\":" << resource.peak_bytes << ',';
        output << "\"reservations\":" << resource.reservations << ',';
        output << "\"releases\":" << resource.releases << ',';
        output << "\"rejected_reservations\":"
               << resource.rejected_reservations << ',';
        output << "\"accounting_errors\":" << resource.accounting_errors << ',';
        output << "\"cache_hits\":" << resource.cache_hits << ',';
        output << "\"cache_misses\":" << resource.cache_misses << ',';
        output << "\"evictions\":" << resource.evictions << ',';
        output << "\"physical_read_bytes\":" << resource.physical_read_bytes << ',';
        output << "\"physical_write_bytes\":" << resource.physical_write_bytes;
        output << '}';
    }

    output << "},\"rust_shadow\":" << rust_shadow_json() << '}';
    return output.str();
}

bool ResourceLedger::rust_shadow_enabled() const noexcept {
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    return rust_shadow_initialized_;
#else
    return false;
#endif
}

bool ResourceLedger::rust_shadow_healthy() const noexcept {
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    return rust_shadow_initialized_ && rust_shadow_mismatches_ == 0U;
#else
    return false;
#endif
}

bool ResourceLedger::verify_rust_shadow() noexcept {
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    if (!rust_shadow_initialized_) {
        return false;
    }

    increment_saturating(rust_shadow_verifications_);
    bool matches = true;
    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        const auto resource_class = static_cast<ResourceClass>(index);
        ZrResourceSnapshot shadow{};
        if (zr_ledger_snapshot(
                &rust_shadow_storage_,
                static_cast<std::uint32_t>(index),
                &shadow) == 0U) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::SnapshotUnavailable,
                resource_class,
                RustShadowSnapshotField::None,
                1U,
                0U);
            matches = false;
            continue;
        }
        matches = rust_shadow_compare_snapshot(
                      resource_class,
                      resources_[index],
                      shadow) &&
            matches;
    }

    const std::size_t shadow_current =
        zr_ledger_total_current_bytes(&rust_shadow_storage_);
    if (shadow_current != total_current_bytes_) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::TotalCurrentBytes,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            static_cast<std::uint64_t>(total_current_bytes_),
            static_cast<std::uint64_t>(shadow_current));
        matches = false;
    }

    const std::size_t shadow_peak =
        zr_ledger_total_peak_bytes(&rust_shadow_storage_);
    if (shadow_peak != total_peak_bytes_) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::TotalPeakBytes,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            static_cast<std::uint64_t>(total_peak_bytes_),
            static_cast<std::uint64_t>(shadow_peak));
        matches = false;
    }

    const bool primary_limits = within_hard_limits();
    const bool shadow_limits =
        zr_ledger_within_hard_limits(&rust_shadow_storage_) != 0U;
    if (primary_limits != shadow_limits) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::WithinHardLimits,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            primary_limits ? 1U : 0U,
            shadow_limits ? 1U : 0U);
        matches = false;
    }

    const bool primary_accounting = accounting_clean();
    const bool shadow_accounting =
        zr_ledger_accounting_clean(&rust_shadow_storage_) != 0U;
    if (primary_accounting != shadow_accounting) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::AccountingClean,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            primary_accounting ? 1U : 0U,
            shadow_accounting ? 1U : 0U);
        matches = false;
    }

    return matches;
#else
    return false;
#endif
}

std::uint64_t ResourceLedger::rust_shadow_operations() const noexcept {
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    return rust_shadow_operations_;
#else
    return 0U;
#endif
}

std::uint64_t ResourceLedger::rust_shadow_verifications() const noexcept {
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    return rust_shadow_verifications_;
#else
    return 0U;
#endif
}

std::uint64_t ResourceLedger::rust_shadow_mismatches() const noexcept {
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    return rust_shadow_mismatches_;
#else
    return 0U;
#endif
}

std::string ResourceLedger::rust_shadow_json() const {
#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
    const char* mismatch_name = "none";
    switch (rust_shadow_first_mismatch_) {
    case RustShadowMismatchKind::None:
        break;
    case RustShadowMismatchKind::RustUnavailable:
        mismatch_name = "rust_unavailable";
        break;
    case RustShadowMismatchKind::AbiVersion:
        mismatch_name = "abi_version";
        break;
    case RustShadowMismatchKind::ResourceClassCount:
        mismatch_name = "resource_class_count";
        break;
    case RustShadowMismatchKind::OperationResult:
        mismatch_name = "operation_result";
        break;
    case RustShadowMismatchKind::SnapshotUnavailable:
        mismatch_name = "snapshot_unavailable";
        break;
    case RustShadowMismatchKind::SnapshotField:
        mismatch_name = "snapshot_field";
        break;
    case RustShadowMismatchKind::TotalCurrentBytes:
        mismatch_name = "total_current_bytes";
        break;
    case RustShadowMismatchKind::TotalPeakBytes:
        mismatch_name = "total_peak_bytes";
        break;
    case RustShadowMismatchKind::WithinHardLimits:
        mismatch_name = "within_hard_limits";
        break;
    case RustShadowMismatchKind::AccountingClean:
        mismatch_name = "accounting_clean";
        break;
    }

    const char* field_name = "none";
    switch (rust_shadow_first_field_) {
    case RustShadowSnapshotField::None:
        break;
    case RustShadowSnapshotField::HardLimitBytes:
        field_name = "hard_limit_bytes";
        break;
    case RustShadowSnapshotField::CurrentBytes:
        field_name = "current_bytes";
        break;
    case RustShadowSnapshotField::PeakBytes:
        field_name = "peak_bytes";
        break;
    case RustShadowSnapshotField::Reservations:
        field_name = "reservations";
        break;
    case RustShadowSnapshotField::Releases:
        field_name = "releases";
        break;
    case RustShadowSnapshotField::RejectedReservations:
        field_name = "rejected_reservations";
        break;
    case RustShadowSnapshotField::AccountingErrors:
        field_name = "accounting_errors";
        break;
    case RustShadowSnapshotField::CacheHits:
        field_name = "cache_hits";
        break;
    case RustShadowSnapshotField::CacheMisses:
        field_name = "cache_misses";
        break;
    case RustShadowSnapshotField::Evictions:
        field_name = "evictions";
        break;
    case RustShadowSnapshotField::PhysicalReadBytes:
        field_name = "physical_read_bytes";
        break;
    case RustShadowSnapshotField::PhysicalWriteBytes:
        field_name = "physical_write_bytes";
        break;
    }

    std::ostringstream output;
    output << "{\"schema\":\"zevryon.rust-shadow-ledger.v1\","
           << "\"enabled\":" << (rust_shadow_initialized_ ? "true" : "false") << ','
           << "\"strict\":"
           << (ZEVRYON_RUST_LEDGER_SHADOW_STRICT != 0 ? "true" : "false") << ','
           << "\"verification_interval\":"
           << static_cast<std::uint64_t>(ZEVRYON_RUST_LEDGER_SHADOW_INTERVAL) << ','
           << "\"abi_version\":" << zr_abi_version() << ','
           << "\"resource_class_count\":" << zr_resource_class_count() << ','
           << "\"operations\":" << rust_shadow_operations_ << ','
           << "\"verifications\":" << rust_shadow_verifications_ << ','
           << "\"mismatches\":" << rust_shadow_mismatches_ << ','
           << "\"healthy\":" << (rust_shadow_healthy() ? "true" : "false") << ','
           << "\"first_mismatch\":\"" << mismatch_name << "\","
           << "\"first_field\":\"" << field_name << "\","
           << "\"first_resource_class\":\""
           << resource_class_name(rust_shadow_first_resource_class_) << "\","
           << "\"expected\":" << rust_shadow_expected_ << ','
           << "\"actual\":" << rust_shadow_actual_ << '}';
    return output.str();
#else
    return "{\"schema\":\"zevryon.rust-shadow-ledger.v1\",\"enabled\":false,"
           "\"strict\":false,\"verification_interval\":0,\"abi_version\":0,"
           "\"resource_class_count\":0,\"operations\":0,\"verifications\":0,"
           "\"mismatches\":0,\"healthy\":false,\"first_mismatch\":\"none\","
           "\"first_field\":\"none\",\"first_resource_class\":\"source_window\","
           "\"expected\":0,\"actual\":0}";
#endif
}

std::size_t ResourceLedger::index_of(ResourceClass resource_class) noexcept {
    const std::size_t index = static_cast<std::size_t>(resource_class);
    return index < resource_class_count ? index : 0U;
}

std::uint64_t ResourceLedger::saturating_add(
    std::uint64_t left,
    std::uint64_t right) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

#if defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
void ResourceLedger::rust_shadow_set_hard_limit(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    if (rust_shadow_initialized_ &&
        zr_ledger_set_hard_limit(
            &rust_shadow_storage_,
            static_cast<std::uint32_t>(index_of(resource_class)),
            bytes) == 0U) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            resource_class,
            RustShadowSnapshotField::HardLimitBytes,
            1U,
            0U);
    }
    rust_shadow_complete_operation(resource_class);
}

void ResourceLedger::rust_shadow_try_reserve(
    ResourceClass resource_class,
    std::size_t bytes,
    bool primary_result) noexcept {
    if (rust_shadow_initialized_) {
        const bool shadow_result =
            zr_ledger_try_reserve(
                &rust_shadow_storage_,
                static_cast<std::uint32_t>(index_of(resource_class)),
                bytes) != 0U;
        if (shadow_result != primary_result) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::OperationResult,
                resource_class,
                RustShadowSnapshotField::Reservations,
                primary_result ? 1U : 0U,
                shadow_result ? 1U : 0U);
        }
    }
    rust_shadow_complete_operation(resource_class);
}

void ResourceLedger::rust_shadow_release(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    if (rust_shadow_initialized_ &&
        zr_ledger_release(
            &rust_shadow_storage_,
            static_cast<std::uint32_t>(index_of(resource_class)),
            bytes) == 0U) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            resource_class,
            RustShadowSnapshotField::Releases,
            1U,
            0U);
    }
    rust_shadow_complete_operation(resource_class);
}

void ResourceLedger::rust_shadow_record_cache_hit(
    ResourceClass resource_class) noexcept {
    if (rust_shadow_initialized_ &&
        zr_ledger_record_cache_hit(
            &rust_shadow_storage_,
            static_cast<std::uint32_t>(index_of(resource_class))) == 0U) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            resource_class,
            RustShadowSnapshotField::CacheHits,
            1U,
            0U);
    }
    rust_shadow_complete_operation(resource_class);
}

void ResourceLedger::rust_shadow_record_cache_miss(
    ResourceClass resource_class) noexcept {
    if (rust_shadow_initialized_ &&
        zr_ledger_record_cache_miss(
            &rust_shadow_storage_,
            static_cast<std::uint32_t>(index_of(resource_class))) == 0U) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            resource_class,
            RustShadowSnapshotField::CacheMisses,
            1U,
            0U);
    }
    rust_shadow_complete_operation(resource_class);
}

void ResourceLedger::rust_shadow_record_eviction(
    ResourceClass resource_class) noexcept {
    if (rust_shadow_initialized_ &&
        zr_ledger_record_eviction(
            &rust_shadow_storage_,
            static_cast<std::uint32_t>(index_of(resource_class))) == 0U) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            resource_class,
            RustShadowSnapshotField::Evictions,
            1U,
            0U);
    }
    rust_shadow_complete_operation(resource_class);
}

void ResourceLedger::rust_shadow_record_physical_read(
    ResourceClass resource_class,
    std::uint64_t bytes) noexcept {
    if (rust_shadow_initialized_ &&
        zr_ledger_record_physical_read(
            &rust_shadow_storage_,
            static_cast<std::uint32_t>(index_of(resource_class)),
            bytes) == 0U) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            resource_class,
            RustShadowSnapshotField::PhysicalReadBytes,
            1U,
            0U);
    }
    rust_shadow_complete_operation(resource_class);
}

void ResourceLedger::rust_shadow_record_physical_write(
    ResourceClass resource_class,
    std::uint64_t bytes) noexcept {
    if (rust_shadow_initialized_ &&
        zr_ledger_record_physical_write(
            &rust_shadow_storage_,
            static_cast<std::uint32_t>(index_of(resource_class)),
            bytes) == 0U) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            resource_class,
            RustShadowSnapshotField::PhysicalWriteBytes,
            1U,
            0U);
    }
    rust_shadow_complete_operation(resource_class);
}

void ResourceLedger::rust_shadow_complete_operation(
    ResourceClass resource_class) noexcept {
    static_cast<void>(resource_class);
    increment_saturating(rust_shadow_operations_);
    constexpr std::uint64_t interval =
        static_cast<std::uint64_t>(ZEVRYON_RUST_LEDGER_SHADOW_INTERVAL);
    if (rust_shadow_initialized_ && interval != 0U &&
        rust_shadow_operations_ % interval == 0U) {
        static_cast<void>(verify_rust_shadow());
    }
}

void ResourceLedger::rust_shadow_record_mismatch(
    RustShadowMismatchKind kind,
    ResourceClass resource_class,
    RustShadowSnapshotField field,
    std::uint64_t expected,
    std::uint64_t actual) noexcept {
    increment_saturating(rust_shadow_mismatches_);
    if (rust_shadow_first_mismatch_ == RustShadowMismatchKind::None) {
        rust_shadow_first_mismatch_ = kind;
        rust_shadow_first_field_ = field;
        rust_shadow_first_resource_class_ = resource_class;
        rust_shadow_expected_ = expected;
        rust_shadow_actual_ = actual;
    }
#if ZEVRYON_RUST_LEDGER_SHADOW_STRICT
    std::abort();
#endif
}

bool ResourceLedger::rust_shadow_compare_snapshot(
    ResourceClass resource_class,
    const ResourceSnapshot& primary,
    const ZrResourceSnapshot& shadow) noexcept {
    bool matches = true;
    const auto compare = [this, resource_class, &matches](
                             RustShadowSnapshotField field,
                             std::uint64_t expected,
                             std::uint64_t actual) noexcept {
        if (expected != actual) {
            rust_shadow_record_mismatch(
                RustShadowMismatchKind::SnapshotField,
                resource_class,
                field,
                expected,
                actual);
            matches = false;
        }
    };

    compare(
        RustShadowSnapshotField::HardLimitBytes,
        static_cast<std::uint64_t>(primary.hard_limit_bytes),
        static_cast<std::uint64_t>(shadow.hard_limit_bytes));
    compare(
        RustShadowSnapshotField::CurrentBytes,
        static_cast<std::uint64_t>(primary.current_bytes),
        static_cast<std::uint64_t>(shadow.current_bytes));
    compare(
        RustShadowSnapshotField::PeakBytes,
        static_cast<std::uint64_t>(primary.peak_bytes),
        static_cast<std::uint64_t>(shadow.peak_bytes));
    compare(RustShadowSnapshotField::Reservations, primary.reservations, shadow.reservations);
    compare(RustShadowSnapshotField::Releases, primary.releases, shadow.releases);
    compare(
        RustShadowSnapshotField::RejectedReservations,
        primary.rejected_reservations,
        shadow.rejected_reservations);
    compare(
        RustShadowSnapshotField::AccountingErrors,
        primary.accounting_errors,
        shadow.accounting_errors);
    compare(RustShadowSnapshotField::CacheHits, primary.cache_hits, shadow.cache_hits);
    compare(RustShadowSnapshotField::CacheMisses, primary.cache_misses, shadow.cache_misses);
    compare(RustShadowSnapshotField::Evictions, primary.evictions, shadow.evictions);
    compare(
        RustShadowSnapshotField::PhysicalReadBytes,
        primary.physical_read_bytes,
        shadow.physical_read_bytes);
    compare(
        RustShadowSnapshotField::PhysicalWriteBytes,
        primary.physical_write_bytes,
        shadow.physical_write_bytes);
    return matches;
}

void ResourceLedger::increment_saturating(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}
#endif

} // namespace zevryon::core
