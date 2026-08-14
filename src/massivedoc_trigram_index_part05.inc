    if (!impl_->index->read_exact_at(0U, header, error)) {
        *error = "cannot read trigram header: " + *error;
        return false;
    }
    for (std::size_t index_magic = 0U; index_magic < kIndexMagic.size(); ++index_magic) {
        if (header[index_magic] != static_cast<std::byte>(kIndexMagic[index_magic])) {
            *error = "trigram index magic mismatch";
            return false;
        }
    }
    const auto version = read_le<std::uint32_t>(header, 8U);
    const auto header_bytes = read_le<std::uint32_t>(header, 12U);
    const auto bloom_bytes = read_le<std::uint32_t>(header, 16U);
    const auto max_streams = read_le<std::uint32_t>(header, 20U);
    const auto blocks = read_le<std::uint64_t>(header, 24U);
    const auto distinct = read_le<std::uint64_t>(header, 32U);
    const auto pairs = read_le<std::uint64_t>(header, 40U);
    const auto directory_offset = read_le<std::uint64_t>(header, 48U);
    const auto postings_offset = read_le<std::uint64_t>(header, 56U);
    const auto postings_bytes = read_le<std::uint64_t>(header, 64U);
    const auto stored_crc = read_le<std::uint32_t>(header, 104U);
    const auto reserved = read_le<std::uint32_t>(header, 108U);
    if (!version || !header_bytes || !bloom_bytes || !max_streams || !blocks || !distinct ||
        !pairs || !directory_offset || !postings_offset || !postings_bytes || !stored_crc || !reserved) {
        *error = "trigram header parse failure";
        return false;
    }
    if (*version != kIndexVersion || *header_bytes != kHeaderBytes ||
        *bloom_bytes != kBloomFrameBytes || *max_streams != kTrigramIntersectionStreams ||
        *directory_offset != kHeaderBytes || *reserved != 0U) {
        *error = "unsupported trigram index header contract";
        return false;
    }
    const std::uint32_t actual_crc = crc32_update(0U, std::span<const std::byte>(header.data(), 104U));
    if (actual_crc != *stored_crc) {
        *error = "trigram header CRC mismatch";
        return false;
    }
    for (std::size_t index = 0U; index < expected_source_identity.size(); ++index) {
        if (std::to_integer<std::uint8_t>(header[72U + index]) != expected_source_identity[index]) {
            *error = "trigram source identity mismatch";
            return false;
        }
    }
    if (*distinct > (std::numeric_limits<std::uint64_t>::max() - *directory_offset) / kDirectoryEntryBytes) {
        *error = "trigram directory extent overflow";
        return false;
    }
    const std::uint64_t expected_postings_offset =
        *directory_offset + *distinct * static_cast<std::uint64_t>(kDirectoryEntryBytes);
    if (*postings_offset != expected_postings_offset ||
        *postings_bytes > std::numeric_limits<std::uint64_t>::max() - *postings_offset ||
        *postings_offset + *postings_bytes != impl_->index->file_size()) {
        *error = "trigram index extent mismatch";
        return false;
    }
    if (*blocks >
            (std::numeric_limits<std::uint64_t>::max() - kBloomHeaderBytes) / kBloomFrameBytes ||
        impl_->bloom->file_size() !=
            kBloomHeaderBytes + *blocks * static_cast<std::uint64_t>(kBloomFrameBytes)) {
        *error = "trigram Bloom extent mismatch";
        return false;
    }
    std::array<std::byte, kBloomHeaderBytes> bloom_header{};
    if (!impl_->bloom->read_exact_at(0U, bloom_header, error)) {
        *error = "cannot read trigram Bloom header: " + *error;
        return false;
    }
    for (std::size_t index = 0U; index < kBloomMagic.size(); ++index) {
        if (bloom_header[index] != static_cast<std::byte>(kBloomMagic[index])) {
            *error = "trigram Bloom magic mismatch";
            return false;
        }
    }
    const auto bloom_version = read_le<std::uint32_t>(bloom_header, 8U);
    const auto bloom_header_bytes = read_le<std::uint32_t>(bloom_header, 12U);
    const auto bloom_frame_bytes = read_le<std::uint32_t>(bloom_header, 16U);
    const auto bloom_reserved0 = read_le<std::uint32_t>(bloom_header, 20U);
    const auto bloom_blocks = read_le<std::uint64_t>(bloom_header, 24U);
    const auto bloom_header_crc = read_le<std::uint32_t>(bloom_header, 64U);
    const auto bloom_reserved1 = read_le<std::uint32_t>(bloom_header, 68U);
    const auto bloom_reserved2 = read_le<std::uint32_t>(bloom_header, 72U);
    const auto bloom_reserved3 = read_le<std::uint32_t>(bloom_header, 76U);
    if (!bloom_version || !bloom_header_bytes || !bloom_frame_bytes || !bloom_reserved0 ||
        !bloom_blocks || !bloom_header_crc || !bloom_reserved1 || !bloom_reserved2 || !bloom_reserved3 ||
        *bloom_version != kBloomVersion || *bloom_header_bytes != kBloomHeaderBytes ||
        *bloom_frame_bytes != kBloomFrameBytes || *bloom_reserved0 != 0U ||
        *bloom_blocks != *blocks || *bloom_reserved1 != 0U || *bloom_reserved2 != 0U ||
        *bloom_reserved3 != 0U ||
        crc32_update(0U, std::span<const std::byte>(bloom_header.data(), 64U)) != *bloom_header_crc) {
        *error = "invalid trigram Bloom header contract";
        return false;
    }
    for (std::size_t index = 0U; index < expected_source_identity.size(); ++index) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(bloom_header[32U + index]);
        if (value != expected_source_identity[index]) {
            *error = "trigram Bloom source identity mismatch";
            return false;
        }
    }

    impl_->block_count = *blocks;
    impl_->distinct_trigrams = *distinct;
    impl_->posting_pairs = *pairs;
    impl_->directory_offset = *directory_offset;
    impl_->postings_offset = *postings_offset;
    impl_->postings_bytes = *postings_bytes;

    impl_->opened = true;
    return true;
}

namespace {

class PostingCursor {
public:
    PostingCursor(
        const BoundedPositionalReader* reader,
        std::uint64_t absolute_offset,
        std::uint64_t byte_length,
        std::uint64_t expected_count,
        std::uint64_t block_count,
        std::size_t window_bytes)
        : reader_(reader),
          end_(absolute_offset + byte_length),
          next_offset_(absolute_offset),
          expected_count_(expected_count),
          block_count_(block_count),
          buffer_(std::max<std::size_t>(1U, std::min<std::size_t>(window_bytes, 4096U))) {}

    bool initialize(std::string* error) {
        return advance(error);
    }

    bool advance(std::string* error) {
        if (decoded_count_ >= expected_count_) {
            valid_ = false;
            if (next_offset_ != end_) {
                *error = "trigram posting stream has trailing bytes";
                return false;
            }
            return true;
        }
        std::uint64_t delta = 0U;
        unsigned int shift = 0U;
        bool complete = false;
        for (unsigned int byte_number = 0U; byte_number < 10U; ++byte_number) {
            unsigned char byte = 0U;
            if (!read_byte(&byte, error)) {
                return false;
            }
            const std::uint64_t low = static_cast<std::uint64_t>(byte & 0x7fU);
            if (shift >= 64U || (shift == 63U && low > 1U)) {
                *error = "trigram posting varint overflow";
                return false;
            }
            delta |= low << shift;
            if ((byte & 0x80U) == 0U) {
                complete = true;
                break;
            }
            shift += 7U;
        }
        if (!complete) {
            *error = "unterminated trigram posting varint";
            return false;
        }
        std::uint64_t block = delta;
        if (decoded_count_ != 0U) {
            if (delta == 0U || current_ > std::numeric_limits<std::uint64_t>::max() - delta) {
                *error = "invalid trigram posting delta";
                return false;
            }
            block = current_ + delta;
        }
        if (block >= block_count_) {
            *error = "trigram posting block out of range";
            return false;
        }
        current_ = block;
        ++decoded_count_;
        valid_ = true;
        if (decoded_count_ == expected_count_ && next_offset_ != end_) {
            // Exact trailing-byte validation is deferred until the next advance so callers can observe the final value.
        }
        return true;
    }

    bool valid() const noexcept { return valid_; }
    std::uint64_t current() const noexcept { return current_; }

private:
    bool fill(std::string* error) {
        if (next_offset_ >= end_) {
            *error = "truncated trigram posting stream";
            return false;
        }
        const std::uint64_t remaining = end_ - next_offset_;
        const std::size_t amount = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer_.size()));
        if (!reader_->read_exact_at(
                next_offset_,
                std::span<std::byte>(buffer_.data(), amount),
                error)) {
            *error = "cannot read trigram posting stream: " + *error;
            return false;
        }
        buffer_position_ = 0U;
        buffer_size_ = amount;
        return true;
    }

    bool read_byte(unsigned char* byte, std::string* error) {
