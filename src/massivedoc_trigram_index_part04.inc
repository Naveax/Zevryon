            } else {
                high = middle;
            }
        }
        if (low >= distinct_trigrams) {
            *found = false;
            return true;
        }
        DirectoryEntry candidate;
        if (!read_directory(low, &candidate, error)) {
            return false;
        }
        if (candidate.trigram != trigram) {
            *found = false;
            return true;
        }
        *entry = candidate;
        *found = true;
        return true;
    }

    bool validate_postings(const DirectoryEntry& entry, std::string* error) const {
        if (entry.postings_offset > postings_bytes ||
            entry.postings_bytes > postings_bytes - entry.postings_offset ||
            postings_offset > std::numeric_limits<std::uint64_t>::max() - entry.postings_offset) {
            *error = "trigram posting directory extent overflow";
            return false;
        }
        const std::size_t window = std::max<std::size_t>(
            1U, std::min<std::size_t>(config.io_window_bytes, 64U * 1024U));
        std::vector<std::byte> buffer(window);
        std::uint64_t offset = postings_offset + entry.postings_offset;
        std::uint64_t remaining = entry.postings_bytes;
        std::uint32_t crc = 0U;
        while (remaining != 0U) {
            const std::size_t amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer.size()));
            if (!index->read_exact_at(
                    offset, std::span<std::byte>(buffer.data(), amount), error)) {
                *error = "cannot validate trigram posting stream: " + *error;
                return false;
            }
            crc = crc32_update(crc, std::span<const std::byte>(buffer.data(), amount));
            offset += static_cast<std::uint64_t>(amount);
            remaining -= static_cast<std::uint64_t>(amount);
        }
        if (crc != entry.postings_crc32) {
            *error = "trigram posting checksum mismatch";
            return false;
        }
        return true;
    }

    bool bloom_allows(
        std::uint64_t block,
        std::string_view query,
        bool* allowed,
        std::string* error) const {
        *allowed = false;
        if (block >= block_count) {
            *error = "trigram Bloom block out of range";
            return false;
        }
        if (block > std::numeric_limits<std::uint64_t>::max() / kBloomFrameBytes) {
            *error = "trigram Bloom offset overflow";
            return false;
        }
        std::array<std::byte, kBloomFrameBytes> frame{};
        if (!bloom->read_exact_at(block * static_cast<std::uint64_t>(kBloomFrameBytes), frame, error)) {
            *error = "cannot read trigram Bloom summary: " + *error;
            return false;
        }
        const auto stored_crc = read_le<std::uint32_t>(frame, kTrigramBloomBytes);
        if (!stored_crc || crc32_update(0U, std::span<const std::byte>(frame.data(), kTrigramBloomBytes)) != *stored_crc) {
            *error = "trigram Bloom checksum mismatch";
            return false;
        }
        const auto bytes = std::span<const std::byte>(frame.data(), kTrigramBloomBytes);
        if (query.size() < 3U) {
            *allowed = true;
            return true;
        }
        for (std::size_t index_query = 2U; index_query < query.size(); ++index_query) {
            const auto trigram = make_trigram(
                static_cast<unsigned char>(query[index_query - 2U]),
                static_cast<unsigned char>(query[index_query - 1U]),
                static_cast<unsigned char>(query[index_query]));
            if (!bloom_has(bytes, trigram)) {
                return true;
            }
        }
        *allowed = true;
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
        *bloom_bytes != kBloomFrameBytes || *max_streams != kTrigramIntersectionStreams ||
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
    if (*blocks > std::numeric_limits<std::uint64_t>::max() / kBloomFrameBytes ||
        impl_->bloom->file_size() != *blocks * static_cast<std::uint64_t>(kBloomFrameBytes)) {
        *error = "trigram Bloom extent mismatch";
        return false;
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
