#include "resource_ledger.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#if !defined(ZEVRYON_RESOURCE_LEDGER_RUST_SHADOW)
#error "Rust-authoritative ResourceLedger requires the Rust ledger boundary"
#endif
#if !defined(ZEVRYON_RESOURCE_LEDGER_RUST_AUTHORITATIVE)
#error "This translation unit is only valid for Rust-authoritative builds"
#endif

#ifndef ZEVRYON_RUST_LEDGER_SHADOW_INTERVAL
#define ZEVRYON_RUST_LEDGER_SHADOW_INTERVAL 1024
#endif
#ifndef ZEVRYON_RUST_LEDGER_SHADOW_STRICT
#define ZEVRYON_RUST_LEDGER_SHADOW_STRICT 0
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

ResourceSnapshot from_rust_snapshot(const ZrResourceSnapshot& snapshot) noexcept {
    ResourceSnapshot result{};
    result.hard_limit_bytes = snapshot.hard_limit_bytes;
    result.current_bytes = snapshot.current_bytes;
    result.peak_bytes = snapshot.peak_bytes;
    result.reservations = snapshot.reservations;
    result.releases = snapshot.releases;
    result.rejected_reservations = snapshot.rejected_reservations;
    result.accounting_errors = snapshot.accounting_errors;
    result.cache_hits = snapshot.cache_hits;
    result.cache_misses = snapshot.cache_misses;
    result.evictions = snapshot.evictions;
    result.physical_read_bytes = snapshot.physical_read_bytes;
    result.physical_write_bytes = snapshot.physical_write_bytes;
    return result;
}

[[noreturn]] void abort_authority() noexcept {
    std::abort();
}

} // namespace

const char* resource_class_name(ResourceClass resource_class) noexcept {
    const std::size_t index = static_cast<std::size_t>(resource_class);
    return index < kResourceNames.size() ? kResourceNames[index] : "invalid";
}

ResourceLedger::ResourceLedger() noexcept {
    for (ResourceSnapshot& resource : resources_) {
        resource.hard_limit_bytes = std::numeric_limits<std::size_t>::max();
    }

    rust_shadow_initialized_ = zr_ledger_init(&rust_shadow_storage_) != 0U;
    if (!rust_shadow_initialized_) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::RustUnavailable,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            1U,
            0U);
        abort_authority();
    }

    const std::uint32_t abi_version = zr_abi_version();
    if (abi_version != ZR_ABI_VERSION) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::AbiVersion,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            ZR_ABI_VERSION,
            abi_version);
        abort_authority();
    }

    const std::uint32_t class_count = zr_resource_class_count();
    if (class_count != ZR_RESOURCE_CLASS_COUNT) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::ResourceClassCount,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            ZR_RESOURCE_CLASS_COUNT,
            class_count);
        abort_authority();
    }
}

void ResourceLedger::set_hard_limit(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    resources_[index_of(resource_class)].hard_limit_bytes = bytes;
    rust_shadow_set_hard_limit(resource_class, bytes);
}

bool ResourceLedger::try_reserve(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    if (!rust_shadow_initialized_) {
        abort_authority();
    }

    const bool rust_result =
        zr_ledger_try_reserve(
            &rust_shadow_storage_,
            static_cast<std::uint32_t>(index_of(resource_class)),
            bytes) != 0U;

    ResourceSnapshot& cpp_shadow = resources_[index_of(resource_class)];
    bool cpp_result = true;
    if (bytes > cpp_shadow.hard_limit_bytes ||
        cpp_shadow.current_bytes > cpp_shadow.hard_limit_bytes - bytes ||
        bytes > std::numeric_limits<std::size_t>::max() - total_current_bytes_) {
        increment_saturating(cpp_shadow.rejected_reservations);
        cpp_result = false;
    } else {
        cpp_shadow.current_bytes += bytes;
        cpp_shadow.peak_bytes = std::max(cpp_shadow.peak_bytes, cpp_shadow.current_bytes);
        increment_saturating(cpp_shadow.reservations);
        total_current_bytes_ += bytes;
        total_peak_bytes_ = std::max(total_peak_bytes_, total_current_bytes_);
    }

    if (rust_result != cpp_result) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            resource_class,
            RustShadowSnapshotField::Reservations,
            rust_result ? 1U : 0U,
            cpp_result ? 1U : 0U);
    }
    rust_shadow_complete_operation(resource_class);
    return rust_result;
}

void ResourceLedger::release(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    ResourceSnapshot& cpp_shadow = resources_[index_of(resource_class)];
    increment_saturating(cpp_shadow.releases);
    if (bytes > cpp_shadow.current_bytes || bytes > total_current_bytes_) {
        increment_saturating(cpp_shadow.accounting_errors);
        total_current_bytes_ -= std::min(total_current_bytes_, cpp_shadow.current_bytes);
        cpp_shadow.current_bytes = 0U;
    } else {
        cpp_shadow.current_bytes -= bytes;
        total_current_bytes_ -= bytes;
    }
    rust_shadow_release(resource_class, bytes);
}

void ResourceLedger::record_cache_hit(ResourceClass resource_class) noexcept {
    increment_saturating(resources_[index_of(resource_class)].cache_hits);
    rust_shadow_record_cache_hit(resource_class);
}

void ResourceLedger::record_cache_miss(ResourceClass resource_class) noexcept {
    increment_saturating(resources_[index_of(resource_class)].cache_misses);
    rust_shadow_record_cache_miss(resource_class);
}

void ResourceLedger::record_eviction(ResourceClass resource_class) noexcept {
    increment_saturating(resources_[index_of(resource_class)].evictions);
    rust_shadow_record_eviction(resource_class);
}

void ResourceLedger::record_physical_read(
    ResourceClass resource_class,
    std::uint64_t bytes) noexcept {
    ResourceSnapshot& cpp_shadow = resources_[index_of(resource_class)];
    cpp_shadow.physical_read_bytes = saturating_add(cpp_shadow.physical_read_bytes, bytes);
    rust_shadow_record_physical_read(resource_class, bytes);
}

void ResourceLedger::record_physical_write(
    ResourceClass resource_class,
    std::uint64_t bytes) noexcept {
    ResourceSnapshot& cpp_shadow = resources_[index_of(resource_class)];
    cpp_shadow.physical_write_bytes = saturating_add(cpp_shadow.physical_write_bytes, bytes);
    rust_shadow_record_physical_write(resource_class, bytes);
}

ResourceSnapshot ResourceLedger::snapshot(ResourceClass resource_class) const noexcept {
    if (!rust_shadow_initialized_) {
        abort_authority();
    }
    ZrResourceSnapshot rust_snapshot{};
    if (zr_ledger_snapshot(
            &rust_shadow_storage_,
            static_cast<std::uint32_t>(index_of(resource_class)),
            &rust_snapshot) == 0U) {
        auto* self = const_cast<ResourceLedger*>(this);
        self->rust_shadow_record_mismatch(
            RustShadowMismatchKind::SnapshotUnavailable,
            resource_class,
            RustShadowSnapshotField::None,
            1U,
            0U);
        abort_authority();
    }
    return from_rust_snapshot(rust_snapshot);
}

std::size_t ResourceLedger::total_current_bytes() const noexcept {
    if (!rust_shadow_initialized_) {
        abort_authority();
    }
    return zr_ledger_total_current_bytes(&rust_shadow_storage_);
}

std::size_t ResourceLedger::total_peak_bytes() const noexcept {
    if (!rust_shadow_initialized_) {
        abort_authority();
    }
    return zr_ledger_total_peak_bytes(&rust_shadow_storage_);
}

bool ResourceLedger::within_hard_limits() const noexcept {
    if (!rust_shadow_initialized_) {
        abort_authority();
    }
    return zr_ledger_within_hard_limits(&rust_shadow_storage_) != 0U;
}

bool ResourceLedger::accounting_clean() const noexcept {
    if (!rust_shadow_initialized_) {
        abort_authority();
    }
    return zr_ledger_accounting_clean(&rust_shadow_storage_) != 0U;
}

std::string ResourceLedger::json() const {
    std::ostringstream output;
    output << "{\"schema\":\"zevryon.resource-ledger.v1\","
           << "\"authoritative_backend\":\"rust\","
           << "\"verification_backend\":\"cpp\","
           << "\"total_current_bytes\":" << total_current_bytes() << ','
           << "\"total_peak_bytes\":" << total_peak_bytes() << ','
           << "\"within_hard_limits\":"
           << (within_hard_limits() ? "true" : "false") << ','
           << "\"accounting_clean\":"
           << (accounting_clean() ? "true" : "false") << ','
           << "\"resources\":{";

    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        if (index != 0U) {
            output << ',';
        }
        const ResourceSnapshot resource = snapshot(static_cast<ResourceClass>(index));
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
    return rust_shadow_initialized_;
}

bool ResourceLedger::rust_shadow_healthy() const noexcept {
    return rust_shadow_initialized_ && rust_shadow_mismatches_ == 0U;
}

bool ResourceLedger::verify_rust_shadow() noexcept {
    if (!rust_shadow_initialized_) {
        return false;
    }

    increment_saturating(rust_shadow_verifications_);
    bool matches = true;
    for (std::size_t index = 0U; index < resource_class_count; ++index) {
        const auto resource_class = static_cast<ResourceClass>(index);
        ZrResourceSnapshot rust_snapshot{};
        if (zr_ledger_snapshot(
                &rust_shadow_storage_,
                static_cast<std::uint32_t>(index),
                &rust_snapshot) == 0U) {
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
                      rust_snapshot) &&
            matches;
    }

    const std::size_t rust_current =
        zr_ledger_total_current_bytes(&rust_shadow_storage_);
    if (rust_current != total_current_bytes_) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::TotalCurrentBytes,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            static_cast<std::uint64_t>(rust_current),
            static_cast<std::uint64_t>(total_current_bytes_));
        matches = false;
    }

    const std::size_t rust_peak = zr_ledger_total_peak_bytes(&rust_shadow_storage_);
    if (rust_peak != total_peak_bytes_) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::TotalPeakBytes,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            static_cast<std::uint64_t>(rust_peak),
            static_cast<std::uint64_t>(total_peak_bytes_));
        matches = false;
    }

    bool cpp_limits = true;
    bool cpp_accounting = true;
    for (const ResourceSnapshot& resource : resources_) {
        cpp_limits = cpp_limits &&
            resource.current_bytes <= resource.hard_limit_bytes &&
            resource.peak_bytes <= resource.hard_limit_bytes;
        cpp_accounting = cpp_accounting && resource.accounting_errors == 0U;
    }

    const bool rust_limits =
        zr_ledger_within_hard_limits(&rust_shadow_storage_) != 0U;
    if (rust_limits != cpp_limits) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::WithinHardLimits,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            rust_limits ? 1U : 0U,
            cpp_limits ? 1U : 0U);
        matches = false;
    }

    const bool rust_accounting =
        zr_ledger_accounting_clean(&rust_shadow_storage_) != 0U;
    if (rust_accounting != cpp_accounting) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::AccountingClean,
            ResourceClass::SourceWindow,
            RustShadowSnapshotField::None,
            rust_accounting ? 1U : 0U,
            cpp_accounting ? 1U : 0U);
        matches = false;
    }

    return matches;
}

std::uint64_t ResourceLedger::rust_shadow_operations() const noexcept {
    return rust_shadow_operations_;
}

std::uint64_t ResourceLedger::rust_shadow_verifications() const noexcept {
    return rust_shadow_verifications_;
}

std::uint64_t ResourceLedger::rust_shadow_mismatches() const noexcept {
    return rust_shadow_mismatches_;
}

std::string ResourceLedger::rust_shadow_json() const {
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
           << "\"mode\":\"rust_authoritative\","
           << "\"authoritative\":true,"
           << "\"authoritative_backend\":\"rust\","
           << "\"shadow_backend\":\"cpp\","
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

void ResourceLedger::rust_shadow_set_hard_limit(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    if (!rust_shadow_initialized_ ||
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
    if (!rust_shadow_initialized_) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::RustUnavailable,
            resource_class,
            RustShadowSnapshotField::Reservations,
            1U,
            0U);
        rust_shadow_complete_operation(resource_class);
        return;
    }
    const bool rust_result =
        zr_ledger_try_reserve(
            &rust_shadow_storage_,
            static_cast<std::uint32_t>(index_of(resource_class)),
            bytes) != 0U;
    if (rust_result != primary_result) {
        rust_shadow_record_mismatch(
            RustShadowMismatchKind::OperationResult,
            resource_class,
            RustShadowSnapshotField::Reservations,
            rust_result ? 1U : 0U,
            primary_result ? 1U : 0U);
    }
    rust_shadow_complete_operation(resource_class);
}

void ResourceLedger::rust_shadow_release(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    if (!rust_shadow_initialized_ ||
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
    if (!rust_shadow_initialized_ ||
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
    if (!rust_shadow_initialized_ ||
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
    if (!rust_shadow_initialized_ ||
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
    if (!rust_shadow_initialized_ ||
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
    if (!rust_shadow_initialized_ ||
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
    const ResourceSnapshot& cpp_shadow,
    const ZrResourceSnapshot& rust_authority) noexcept {
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
        static_cast<std::uint64_t>(rust_authority.hard_limit_bytes),
        static_cast<std::uint64_t>(cpp_shadow.hard_limit_bytes));
    compare(
        RustShadowSnapshotField::CurrentBytes,
        static_cast<std::uint64_t>(rust_authority.current_bytes),
        static_cast<std::uint64_t>(cpp_shadow.current_bytes));
    compare(
        RustShadowSnapshotField::PeakBytes,
        static_cast<std::uint64_t>(rust_authority.peak_bytes),
        static_cast<std::uint64_t>(cpp_shadow.peak_bytes));
    compare(
        RustShadowSnapshotField::Reservations,
        rust_authority.reservations,
        cpp_shadow.reservations);
    compare(
        RustShadowSnapshotField::Releases,
        rust_authority.releases,
        cpp_shadow.releases);
    compare(
        RustShadowSnapshotField::RejectedReservations,
        rust_authority.rejected_reservations,
        cpp_shadow.rejected_reservations);
    compare(
        RustShadowSnapshotField::AccountingErrors,
        rust_authority.accounting_errors,
        cpp_shadow.accounting_errors);
    compare(
        RustShadowSnapshotField::CacheHits,
        rust_authority.cache_hits,
        cpp_shadow.cache_hits);
    compare(
        RustShadowSnapshotField::CacheMisses,
        rust_authority.cache_misses,
        cpp_shadow.cache_misses);
    compare(
        RustShadowSnapshotField::Evictions,
        rust_authority.evictions,
        cpp_shadow.evictions);
    compare(
        RustShadowSnapshotField::PhysicalReadBytes,
        rust_authority.physical_read_bytes,
        cpp_shadow.physical_read_bytes);
    compare(
        RustShadowSnapshotField::PhysicalWriteBytes,
        rust_authority.physical_write_bytes,
        cpp_shadow.physical_write_bytes);
    return matches;
}

void ResourceLedger::increment_saturating(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

#if defined(ZEVRYON_RESOURCE_LEDGER_AUTHORITY_TEST_HOOKS)
void ResourceLedger::inject_cpp_shadow_reservation_for_testing(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    ResourceSnapshot& cpp_shadow = resources_[index_of(resource_class)];
    if (bytes > std::numeric_limits<std::size_t>::max() - cpp_shadow.current_bytes ||
        bytes > std::numeric_limits<std::size_t>::max() - total_current_bytes_) {
        abort_authority();
    }
    cpp_shadow.current_bytes += bytes;
    cpp_shadow.peak_bytes = std::max(cpp_shadow.peak_bytes, cpp_shadow.current_bytes);
    increment_saturating(cpp_shadow.reservations);
    total_current_bytes_ += bytes;
    total_peak_bytes_ = std::max(total_peak_bytes_, total_current_bytes_);
}
#endif

} // namespace zevryon::core
