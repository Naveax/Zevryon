        std::array<std::byte, kTrigramBloomBytes> bytes{};
        if (!bloom->read_exact_at(block * static_cast<std::uint64_t>(kTrigramBloomBytes), bytes, error)) {
            *error = "cannot read trigram Bloom summary: " + *error;
            return false;
        }
        if (query.size() < 3U) {
            return true;
        }
        for (std::size_t index_query = 2U; index_query < query.size(); ++index_query) {
            const auto trigram = make_trigram(
                static_cast<unsigned char>(query[index_query - 2U]),
                static_cast<unsigned char>(query[index_query - 1U]),
                static_cast<unsigned char>(query[index_query]));
            if (!bloom_has(bytes, trigram)) {
                return false;
            }
        }
        return true;
    }
};

TrigramIndexReader::TrigramIndexReader(
    std::filesystem::path root,
    TrigramIndexConfig config)
    : impl_(new Impl{}) {
    impl_->root = std::move(root);
    impl_->config = config;
    if (impl_->config.io_window_bytes == 0U) {
        impl_->config.io_window_bytes = 64U * 1024U;
    }
}

TrigramIndexReader::~TrigramIndexReader() {
    delete impl_;
}

bool TrigramIndexReader::open(std::string* error) {
    impl_->opened = false;
    impl_->index = std::make_unique<BoundedPositionalReader>(
        impl_->root / "search.tri",
        impl_->config.io_window_bytes);
    impl_->bloom = std::make_unique<BoundedPositionalReader>(
        impl_->root / "search.blm",
        impl_->config.io_window_bytes);
    if (!impl_->index->open(error)) {
        *error = "cannot open trigram index: " + *error;
        return false;
    }
    if (!impl_->bloom->open(error)) {
        *error = "cannot open trigram Bloom summaries: " + *error;
        return false;
    }
    if (impl_->index->file_size() < kHeaderBytes) {
        *error = "trigram index is shorter than its header";
        return false;
    }
    std::array<std::byte, kHeaderBytes> header{};
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
    const auto stored_crc = read_le<std::uint32_t>(header, 72U);
    const auto reserved = read_le<std::uint32_t>(header, 76U);
    if (!version || !header_bytes || !bloom_bytes || !max_streams || !blocks || !distinct ||
        !pairs || !directory_offset || !postings_offset || !postings_bytes || !stored_crc || !reserved) {
        *error = "trigram header parse failure";
        return false;
    }
    if (*version != kIndexVersion || *header_bytes != kHeaderBytes ||
        *bloom_bytes != kTrigramBloomBytes || *max_streams != kTrigramIntersectionStreams ||
        *directory_offset != kHeaderBytes || *reserved != 0U) {
        *error = "unsupported trigram index header contract";
        return false;
    }
    const std::uint32_t actual_crc = crc32_update(0U, std::span<const std::byte>(header.data(), 72U));
    if (actual_crc != *stored_crc) {
        *error = "trigram header CRC mismatch";
        return false;
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
    if (*blocks > std::numeric_limits<std::uint64_t>::max() / kTrigramBloomBytes ||
        impl_->bloom->file_size() != *blocks * static_cast<std::uint64_t>(kTrigramBloomBytes)) {
        *error = "trigram Bloom extent mismatch";
        return false;
    }

    impl_->block_count = *blocks;
    impl_->distinct_trigrams = *distinct;
    impl_->posting_pairs = *pairs;
    impl_->directory_offset = *directory_offset;
    impl_->postings_offset = *postings_offset;
    impl_->postings_bytes = *postings_bytes;

    std::optional<std::uint32_t> previous_trigram;
    std::uint64_t previous_end = 0U;
    std::uint64_t counted_pairs = 0U;
    for (std::uint64_t index_directory = 0U; index_directory < impl_->distinct_trigrams; ++index_directory) {
        DirectoryEntry entry;
        if (!impl_->read_directory(index_directory, &entry, error)) {
            return false;
        }
        if ((previous_trigram && entry.trigram <= *previous_trigram) ||
            entry.block_count == 0U || entry.postings_bytes == 0U ||
            entry.postings_offset != previous_end ||
            entry.postings_offset > impl_->postings_bytes ||
            entry.postings_bytes > impl_->postings_bytes - entry.postings_offset ||
            counted_pairs > std::numeric_limits<std::uint64_t>::max() - entry.block_count) {
            *error = "invalid trigram directory ordering or extent";
            return false;
        }
        previous_trigram = entry.trigram;
        previous_end = entry.postings_offset + entry.postings_bytes;
        counted_pairs += entry.block_count;
    }
    if (previous_end != impl_->postings_bytes || counted_pairs != impl_->posting_pairs) {
        *error = "trigram directory aggregate mismatch";
        return false;
    }
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
