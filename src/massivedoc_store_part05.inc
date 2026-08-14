bool import_zmdoc_corpus(
    const std::filesystem::path& corpus_path,
    const std::filesystem::path& store_root,
    StoreConfig config,
    StoreStats* stats,
    std::string* error) {
    std::ifstream corpus(corpus_path, std::ios::binary);
    if (!corpus) {
        *error = "cannot open ZMDOC corpus";
        return false;
    }
    std::array<std::byte, 64> header{};
    if (!read_exact(corpus, header)) {
        *error = "truncated ZMDOC header";
        return false;
    }
    for (std::size_t index = 0U; index < kCorpusMagic.size(); ++index) {
        if (std::to_integer<unsigned char>(header[index]) != static_cast<unsigned char>(kCorpusMagic[index])) {
            *error = "invalid ZMDOC magic";
            return false;
        }
    }
    const auto logical_bytes = read_le<std::uint64_t>(header, 8U);
    const auto records = read_le<std::uint64_t>(header, 16U);
    const auto nodes = read_le<std::uint64_t>(header, 24U);
    const auto styles = read_le<std::uint64_t>(header, 32U);
    const auto resources = read_le<std::uint64_t>(header, 40U);
    const auto largest = read_le<std::uint64_t>(header, 48U);
    if (!logical_bytes || !records || !nodes || !styles || !resources || !largest) {
        *error = "invalid ZMDOC header fields";
        return false;
    }
    StoreWriter writer(store_root, config);
    std::array<std::byte, 12> record_header{};
    for (std::uint64_t index = 0U; index < *records; ++index) {
        if (!read_exact(corpus, record_header)) {
            *error = "truncated ZMDOC record header";
            return false;
        }
        const auto logical_id = read_le<std::uint64_t>(record_header, 0U);
        const auto length32 = read_le<std::uint32_t>(record_header, 8U);
        if (!logical_id || !length32) {
            *error = "invalid ZMDOC record header";
            return false;
        }
        std::uint64_t remaining = *length32;
        if (!writer.append_stream(*logical_id, remaining, [&corpus, &remaining](std::span<std::byte> destination) -> std::size_t {
                const std::size_t amount = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(destination.size())));
                if (amount == 0U) {
                    return 0U;
                }
                corpus.read(reinterpret_cast<char*>(destination.data()), static_cast<std::streamsize>(amount));
                const std::size_t received = static_cast<std::size_t>(corpus.gcount());
                remaining -= static_cast<std::uint64_t>(received);
                return received;
            }, error)) {
            return false;
        }
    }
    if (corpus.peek() != std::char_traits<char>::eof()) {
        *error = "ZMDOC corpus has trailing bytes";
        return false;
    }
    CorpusMetadata metadata{*logical_bytes, *records, *nodes, *styles, *resources, *largest};
    return writer.finalize(metadata, stats, error);
}

std::string stats_json(const StoreStats& stats) {
    std::ostringstream output;
    output << "{\"logical_utf8_bytes\":" << stats.corpus.logical_utf8_bytes
           << ",\"logical_records\":" << stats.corpus.logical_records
           << ",\"logical_nodes\":" << stats.corpus.logical_nodes
           << ",\"style_runs\":" << stats.corpus.style_runs
           << ",\"resource_references\":" << stats.corpus.resource_references
           << ",\"largest_record_bytes\":" << stats.corpus.largest_record_bytes
           << ",\"segment_count\":" << stats.segment_count
           << ",\"chunk_count\":" << stats.chunk_count
           << ",\"search_block_count\":" << stats.search_block_count
           << ",\"physical_bytes\":" << stats.physical_bytes
           << ",\"payload_sha256\":\"" << escape_json(stats.payload_sha256) << "\"}";
    return output.str();
}

} // namespace zevryon::massivedoc

#include "massivedoc_store_preview.inc"
#include "massivedoc_store_cold.inc"
