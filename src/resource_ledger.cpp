#include "resource_ledger.hpp"

#include <algorithm>
#include <sstream>

namespace zevryon::core {
namespace {

constexpr std::array<const char*, resource_class_count> kResourceNames{
    "source_window",
    "checkpoint_index",
    "unicode_buffer",
    "grapheme_cluster",
    "script_run",
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
}

void ResourceLedger::set_hard_limit(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    resources_[index_of(resource_class)].hard_limit_bytes = bytes;
}

bool ResourceLedger::try_reserve(
    ResourceClass resource_class,
    std::size_t bytes) noexcept {
    ResourceSnapshot& resource = resources_[index_of(resource_class)];
    if (bytes > resource.hard_limit_bytes ||
        resource.current_bytes > resource.hard_limit_bytes - bytes ||
        bytes > std::numeric_limits<std::size_t>::max() - total_current_bytes_) {
        ++resource.rejected_reservations;
        return false;
    }

    resource.current_bytes += bytes;
    resource.peak_bytes = std::max(resource.peak_bytes, resource.current_bytes);
    ++resource.reservations;
    total_current_bytes_ += bytes;
    total_peak_bytes_ = std::max(total_peak_bytes_, total_current_bytes_);
    return true;
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
        return;
    }

    resource.current_bytes -= bytes;
    total_current_bytes_ -= bytes;
}

void ResourceLedger::record_cache_hit(ResourceClass resource_class) noexcept {
    ++resources_[index_of(resource_class)].cache_hits;
}

void ResourceLedger::record_cache_miss(ResourceClass resource_class) noexcept {
    ++resources_[index_of(resource_class)].cache_misses;
}

void ResourceLedger::record_eviction(ResourceClass resource_class) noexcept {
    ++resources_[index_of(resource_class)].evictions;
}

void ResourceLedger::record_physical_read(
    ResourceClass resource_class,
    std::uint64_t bytes) noexcept {
    ResourceSnapshot& resource = resources_[index_of(resource_class)];
    resource.physical_read_bytes = saturating_add(resource.physical_read_bytes, bytes);
}

void ResourceLedger::record_physical_write(
    ResourceClass resource_class,
    std::uint64_t bytes) noexcept {
    ResourceSnapshot& resource = resources_[index_of(resource_class)];
    resource.physical_write_bytes = saturating_add(resource.physical_write_bytes, bytes);
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

    output << "}}";
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

} // namespace zevryon::core
