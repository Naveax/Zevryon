std::uint64_t StoreReader::cache_resident_bytes() const noexcept {
    return impl_->cache.resident_bytes();
}

std::uint64_t StoreReader::cache_peak_resident_bytes() const noexcept {
    return impl_->cache.peak_resident_bytes();
}

std::uint64_t StoreReader::cache_physical_read_bytes() const noexcept {
    return impl_->cache.physical_read_bytes();
}

std::uint64_t StoreReader::cold_window_resident_bytes() const noexcept {
    return impl_->cold_window.resident_bytes();
}

std::uint64_t StoreReader::cold_window_peak_resident_bytes() const noexcept {
    return impl_->cold_window.peak_resident_bytes();
}

std::uint64_t StoreReader::cold_window_touched_bytes() const noexcept {
    return impl_->cold_window.touched_bytes();
}

bool StoreReader::cache_ledger_within_hard_limits() const noexcept {
    return impl_->cache_ledger.within_hard_limits();
}

bool StoreReader::cache_ledger_accounting_clean() const noexcept {
    return impl_->cache_ledger.accounting_clean();
}

bool StoreReader::cold_window_ledger_within_hard_limits() const noexcept {
    return impl_->cold_window_ledger.within_hard_limits();
}

bool StoreReader::cold_window_ledger_accounting_clean() const noexcept {
    return impl_->cold_window_ledger.accounting_clean();
}

std::string stats_json(const StoreStats& stats, std::uint64_t resident_bytes) {
    std::ostringstream output;
    output << "{\n"
           << "  \"logical_records\": " << stats.corpus.logical_records << ",\n"
           << "  \"logical_bytes\": " << stats.corpus.logical_bytes << ",\n"
           << "  \"chunk_count\": " << stats.chunk_count << ",\n"
           << "  \"segment_count\": " << stats.segment_count << ",\n"
           << "  \"search_block_count\": " << stats.search_block_count << ",\n"
           << "  \"physical_store_bytes\": " << stats.physical_store_bytes << ",\n"
           << "  \"process_resident_bytes\": " << resident_bytes << "\n"
           << "}\n";
    return output.str();
}

#include "massivedoc_store_unicode_search.inc"

} // namespace zevryon::massivedoc
