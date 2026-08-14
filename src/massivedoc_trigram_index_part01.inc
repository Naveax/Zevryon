    bytes.reserve(kDirectoryEntryBytes);
    append_le(bytes, entry.trigram);
    append_le(bytes, std::uint32_t{0U});
    append_le(bytes, entry.postings_offset);
    append_le(bytes, entry.postings_bytes);
    append_le(bytes, entry.block_count);
    return bytes;
}

std::optional<DirectoryEntry> parse_directory(std::span<const std::byte> bytes) {
    if (bytes.size() != kDirectoryEntryBytes) {
        return std::nullopt;
    }
    const auto trigram = read_le<std::uint32_t>(bytes, 0U);
    const auto reserved = read_le<std::uint32_t>(bytes, 4U);
    const auto offset = read_le<std::uint64_t>(bytes, 8U);
    const auto posting_bytes = read_le<std::uint64_t>(bytes, 16U);
    const auto count = read_le<std::uint64_t>(bytes, 24U);
    if (!trigram || !reserved || !offset || !posting_bytes || !count || *reserved != 0U) {
        return std::nullopt;
    }
    return DirectoryEntry{*trigram, *offset, *posting_bytes, *count};
}

void encode_varint(std::uint64_t value, std::vector<std::byte>* output) {
    do {
        unsigned char byte = static_cast<unsigned char>(value & 0x7fU);
        value >>= 7U;
        if (value != 0U) {
            byte = static_cast<unsigned char>(byte | 0x80U);
        }
        output->push_back(static_cast<std::byte>(byte));
    } while (value != 0U);
}

struct RunNode {
    SpoolEntry entry;
    std::size_t source{0U};
};

struct RunNodeGreater {
    bool operator()(const RunNode& left, const RunNode& right) const {
        if (left.entry.trigram != right.entry.trigram) {
            return left.entry.trigram > right.entry.trigram;
        }
        if (left.entry.block != right.entry.block) {
            return left.entry.block > right.entry.block;
        }
        return left.source > right.source;
    }
};

bool merge_run_group(
    const std::vector<std::filesystem::path>& inputs,
    const std::filesystem::path& output,
    const TrigramCancellationCheck& cancelled,
    std::string* error) {
    std::vector<std::ifstream> streams;
    streams.reserve(inputs.size());
    for (const auto& path : inputs) {
        streams.emplace_back(path, std::ios::binary);
        if (!streams.back()) {
            *error = "cannot open trigram merge input";
            return false;
        }
    }
    std::ofstream destination(output, std::ios::binary | std::ios::trunc);
    if (!destination) {
        *error = "cannot create trigram merge output";
        return false;
    }
    std::priority_queue<RunNode, std::vector<RunNode>, RunNodeGreater> heap;
    for (std::size_t index = 0U; index < streams.size(); ++index) {
        SpoolEntry entry;
        bool eof = false;
        if (!read_spool_entry(streams[index], &entry, &eof, error)) {
            return false;
        }
        if (!eof) {
            heap.push(RunNode{entry, index});
        }
    }
    std::optional<SpoolEntry> previous;
    std::uint64_t emitted = 0U;
    while (!heap.empty()) {
        if ((emitted & 0xfffU) == 0U && cancellation_requested(cancelled)) {
            *error = "trigram index build cancelled";
            return false;
        }
        const RunNode node = heap.top();
        heap.pop();
        if (!previous || !entry_equal(*previous, node.entry)) {
            if (!write_spool_entry(destination, node.entry, error)) {
                return false;
            }
            previous = node.entry;
            ++emitted;
        }
        SpoolEntry next;
        bool eof = false;
        if (!read_spool_entry(streams[node.source], &next, &eof, error)) {
            return false;
        }
        if (!eof) {
            heap.push(RunNode{next, node.source});
        }
    }
    destination.flush();
    if (!destination) {
        *error = "failed to flush trigram merge output";
        return false;
    }
    return true;
}

} // namespace

struct TrigramIndexWriter::Impl {
    std::filesystem::path root;
    TrigramIndexConfig config;
    std::filesystem::path temp_root;
    std::filesystem::path spool_path;
    std::filesystem::path bloom_temp_path;
    std::ofstream spool;
    std::ofstream bloom;
    std::array<std::byte, kTrigramBloomBytes> current_bloom{};
    std::uint64_t current_block{0U};
    bool have_block{false};
    bool in_record{false};
    std::array<unsigned char, 2> previous{};
    std::size_t previous_count{0U};
    std::uint64_t spool_entries{0U};
    bool finished{false};

    bool flush_bloom(std::string* error) {
        if (!have_block) {
            return true;
        }
        if (!write_all(bloom, current_bloom)) {
            *error = "failed to write trigram block Bloom summary";
            return false;
        }
        current_bloom.fill(std::byte{0});
        return true;
    }
};

TrigramIndexWriter::TrigramIndexWriter(
    std::filesystem::path root,
    TrigramIndexConfig config)
    : impl_(new Impl{}) {
    impl_->root = std::move(root);
    impl_->config = config;
    if (impl_->config.io_window_bytes == 0U) {
        impl_->config.io_window_bytes = 64U * 1024U;
    }
    if (impl_->config.sort_memory_bytes < 64U * 1024U) {
        impl_->config.sort_memory_bytes = 64U * 1024U;
    }
    if (impl_->config.merge_fan_in < 2U) {
        impl_->config.merge_fan_in = 2U;
    }
    if (impl_->config.merge_fan_in > 64U) {
        impl_->config.merge_fan_in = 64U;
    }
    impl_->temp_root = impl_->root / ".trigram-build";
    impl_->spool_path = impl_->temp_root / "spool.bin";
    impl_->bloom_temp_path = impl_->temp_root / "search.blm.tmp";
    std::error_code fs_error;
    std::filesystem::remove_all(impl_->temp_root, fs_error);
    fs_error.clear();
    std::filesystem::create_directories(impl_->temp_root, fs_error);
    if (!fs_error) {
        impl_->spool.open(impl_->spool_path, std::ios::binary | std::ios::trunc);
        impl_->bloom.open(impl_->bloom_temp_path, std::ios::binary | std::ios::trunc);
    }
}

TrigramIndexWriter::~TrigramIndexWriter() {
    delete impl_;
}

bool TrigramIndexWriter::begin_record(std::uint64_t block_index, std::string* error) {
    if (impl_ == nullptr || impl_->finished || impl_->in_record) {
        *error = "trigram writer is unavailable, finished, or already inside a record";
        return false;
    }
    if (!impl_->spool || !impl_->bloom) {
        *error = "trigram writer temporary files are not open";
        return false;
    }
    if (!impl_->have_block) {
        if (block_index != 0U) {
            *error = "trigram block sequence must begin at zero";
            return false;
        }
        impl_->current_block = block_index;
        impl_->have_block = true;
    } else if (block_index != impl_->current_block) {
        if (block_index != impl_->current_block + 1U) {
            *error = "trigram block sequence must be contiguous";
            return false;
        }
        if (!impl_->flush_bloom(error)) {
            return false;
        }
        impl_->current_block = block_index;
    }
    impl_->previous_count = 0U;
    impl_->in_record = true;
    return true;
}

bool TrigramIndexWriter::feed(std::span<const std::byte> bytes, std::string* error) {
    if (impl_ == nullptr || !impl_->in_record || impl_->finished) {
        *error = "trigram writer feed requires an active record";
        return false;
    }
    for (const std::byte raw : bytes) {
        const unsigned char current = std::to_integer<unsigned char>(raw);
