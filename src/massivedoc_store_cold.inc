namespace zevryon::massivedoc {

bool StoreReader::touch_record_slice_cold(
    std::uint64_t record_index,
    std::uint64_t byte_offset,
    std::size_t max_bytes,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_ == nullptr || !impl_->opened) {
        *error = "store is not open";
        return false;
    }
    if (impl_->read_config.cold_window_bytes == 0U) {
        *error = "store reader cold mapped window is disabled";
        return false;
    }
    if (!validate_cold_mapped_window_bytes(
            impl_->read_config.cold_window_bytes,
            error)) {
        *error = "store reader cold mapped window config is invalid: " + *error;
        return false;
    }
    if (impl_->read_config.cold_window_bytes <=
        kColdMappedWindowAlignmentSlackBytes) {
        *error = "store reader cold mapped window is too small for portable alignment slack";
        return false;
    }
    if (cold_window_ == nullptr) {
        cold_window_ = std::make_unique<ColdMappedWindow>(
            impl_->read_config.cold_window_bytes);
    }

    RecordDescriptor record;
    if (!impl_->read_record_descriptor(record_index, &record, error)) {
        return false;
    }
    if (byte_offset > record.length) {
        *error = "cold record slice offset out of range";
        return false;
    }

    const std::uint64_t available = record.length - byte_offset;
    std::uint64_t remaining = std::min<std::uint64_t>(
        available,
        static_cast<std::uint64_t>(max_bytes));
    if (remaining == 0U) {
        return true;
    }

    std::uint64_t logical_position = 0U;
    for (std::uint32_t relative = 0U;
         relative < record.chunk_count && remaining != 0U;
         ++relative) {
        if (record.first_chunk >
            std::numeric_limits<std::uint64_t>::max() -
                static_cast<std::uint64_t>(relative)) {
            *error = "cold record slice chunk index overflow";
            return false;
        }
        ChunkDescriptor chunk;
        if (!impl_->read_chunk_descriptor(
                record.first_chunk + static_cast<std::uint64_t>(relative),
                &chunk,
                error)) {
            return false;
        }
        if (chunk.length > std::numeric_limits<std::uint64_t>::max() - logical_position) {
            *error = "cold record slice chunk range overflow";
            return false;
        }
        const std::uint64_t chunk_end = logical_position + chunk.length;
        if (byte_offset >= chunk_end) {
            logical_position = chunk_end;
            continue;
        }

        const std::uint64_t offset_in_chunk =
            byte_offset > logical_position ? byte_offset - logical_position : 0U;
        if (offset_in_chunk > chunk.length ||
            chunk.offset > std::numeric_limits<std::uint64_t>::max() - offset_in_chunk) {
            *error = "cold record slice segment offset overflow";
            return false;
        }
        std::uint64_t segment_offset = chunk.offset + offset_in_chunk;
        std::uint64_t chunk_remaining = chunk.length - offset_in_chunk;
        chunk_remaining = std::min(chunk_remaining, remaining);

        while (chunk_remaining != 0U) {
            const std::size_t prefix = static_cast<std::size_t>(
                segment_offset % kColdMappedWindowAlignmentSlackBytes);
            if (prefix >= impl_->read_config.cold_window_bytes) {
                *error = "cold mapped alignment prefix exceeds configured window";
                return false;
            }
            const std::size_t capacity =
                impl_->read_config.cold_window_bytes - prefix;
            const std::size_t amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    chunk_remaining,
                    static_cast<std::uint64_t>(capacity)));
            if (amount == 0U) {
                *error = "cold mapped window made no forward progress";
                return false;
            }
            if (!cold_window_->touch(
                    segment_path(impl_->root, chunk.segment_id),
                    segment_offset,
                    amount,
                    error)) {
                *error = "cannot retain record slice in cold mapped window: " + *error;
                return false;
            }
            segment_offset += static_cast<std::uint64_t>(amount);
            chunk_remaining -= static_cast<std::uint64_t>(amount);
            remaining -= static_cast<std::uint64_t>(amount);
        }
        logical_position = chunk_end;
    }

    if (remaining != 0U) {
        *error = "cold record slice length mismatch";
        return false;
    }
    return true;
}

ColdMappedWindowStats StoreReader::cold_window_stats() const noexcept {
    return cold_window_ != nullptr ? cold_window_->stats() : ColdMappedWindowStats{};
}

void StoreReader::release_cold_window() noexcept {
    if (cold_window_ != nullptr) {
        cold_window_->release();
    }
}

} // namespace zevryon::massivedoc
