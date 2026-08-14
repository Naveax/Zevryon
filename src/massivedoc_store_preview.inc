#include "massivedoc_progressive_import.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace zevryon::massivedoc {
namespace preview_detail {

constexpr std::size_t kSnapshotCopyBytes = 64U * 1024U;

bool multiply_exact(
    std::uint64_t count,
    std::uint64_t width,
    std::uint64_t* output,
    std::string* error,
    const char* label) {
    if (count != 0U && width > std::numeric_limits<std::uint64_t>::max() / count) {
        *error = std::string(label) + " byte size overflow";
        return false;
    }
    *output = count * width;
    return true;
}

bool file_size_exact(
    const std::filesystem::path& path,
    std::uint64_t expected,
    std::string* error,
    const char* label) {
    std::error_code filesystem_error;
    const std::uintmax_t size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        *error = std::string("cannot size ") + label + ": " + filesystem_error.message();
        return false;
    }
    if (size > std::numeric_limits<std::uint64_t>::max() ||
        static_cast<std::uint64_t>(size) != expected) {
        *error = std::string(label) + " size changed during prefix snapshot";
        return false;
    }
    return true;
}

bool copy_exact_bounded(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::uint64_t expected_bytes,
    std::string* error,
    const char* label) {
    if (!file_size_exact(source, expected_bytes, error, label)) {
        return false;
    }
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        *error = std::string("cannot open ") + label + " for bounded snapshot copy";
        return false;
    }
    std::array<std::byte, kSnapshotCopyBytes> buffer{};
    std::uint64_t remaining = expected_bytes;
    while (remaining != 0U) {
        const std::size_t amount = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(amount));
        if (input.gcount() != static_cast<std::streamsize>(amount)) {
            *error = std::string("short read while snapshotting ") + label;
            return false;
        }
        output.write(
            reinterpret_cast<const char*>(buffer.data()),
            static_cast<std::streamsize>(amount));
        if (!output) {
            *error = std::string("short write while snapshotting ") + label;
            return false;
        }
        remaining -= static_cast<std::uint64_t>(amount);
    }
    output.flush();
    if (!output) {
        *error = std::string("cannot flush bounded snapshot copy for ") + label;
        return false;
    }
    output.close();
    input.close();
    if (output.fail() || input.fail()) {
        *error = std::string("cannot close bounded snapshot copy for ") + label;
        return false;
    }
    return file_size_exact(destination, expected_bytes, error, label);
}

bool append_partial_search_signature(
    const std::filesystem::path& path,
    const std::array<std::byte, kBigramSignatureBytes>& signature,
    std::string* error) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output || !write_all(output, signature)) {
        *error = "cannot append partial search signature to prefix snapshot";
        return false;
    }
    output.flush();
    if (!output) {
        *error = "cannot flush partial search signature in prefix snapshot";
        return false;
    }
    output.close();
    if (output.fail()) {
        *error = "cannot close partial search signature in prefix snapshot";
        return false;
    }
    return true;
}

bool same_path(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    std::error_code left_error;
    std::error_code right_error;
    const auto normalized_left =
        std::filesystem::absolute(left, left_error).lexically_normal();
    const auto normalized_right =
        std::filesystem::absolute(right, right_error).lexically_normal();
    return !left_error && !right_error && normalized_left == normalized_right;
}

bool read_progressive_header(
    std::ifstream& corpus,
    CorpusMetadata* metadata,
    std::uint64_t* records,
    std::string* error) {
    std::array<std::byte, 64> header{};
    if (!read_exact(corpus, header)) {
        *error = "truncated ZMDOC header";
        return false;
    }
    for (std::size_t index = 0U; index < kCorpusMagic.size(); ++index) {
        if (std::to_integer<unsigned char>(header[index]) !=
            static_cast<unsigned char>(kCorpusMagic[index])) {
            *error = "invalid ZMDOC magic";
            return false;
        }
    }
    const auto logical_bytes = read_le<std::uint64_t>(header, 8U);
    const auto logical_records = read_le<std::uint64_t>(header, 16U);
    const auto nodes = read_le<std::uint64_t>(header, 24U);
    const auto styles = read_le<std::uint64_t>(header, 32U);
    const auto resources = read_le<std::uint64_t>(header, 40U);
    const auto largest = read_le<std::uint64_t>(header, 48U);
    if (!logical_bytes || !logical_records || !nodes || !styles || !resources || !largest) {
        *error = "invalid ZMDOC header fields";
        return false;
    }
    *metadata = CorpusMetadata{
        *logical_bytes,
        *logical_records,
        *nodes,
        *styles,
        *resources,
        *largest};
    *records = *logical_records;
    return true;
}

bool append_progressive_record(
    std::ifstream& corpus,
    StoreWriter* writer,
    std::string* error) {
    std::array<std::byte, 12> record_header{};
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
    return writer->append_stream(
        *logical_id,
        remaining,
        [&corpus, &remaining](std::span<std::byte> destination) -> std::size_t {
            const std::size_t amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    remaining,
                    static_cast<std::uint64_t>(destination.size())));
            if (amount == 0U) {
                return 0U;
            }
            corpus.read(
                reinterpret_cast<char*>(destination.data()),
                static_cast<std::streamsize>(amount));
            const std::size_t received = static_cast<std::size_t>(corpus.gcount());
            remaining -= static_cast<std::uint64_t>(received);
            return received;
        },
        error);
}

} // namespace preview_detail

bool StoreWriter::snapshot_prefix(
    const std::filesystem::path& snapshot_root,
    StoreStats* stats,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_ == nullptr || impl_->finalized) {
        *error = "store writer is unavailable or finalized";
        return false;
    }
    if (impl_->record_count == 0U) {
        *error = "cannot snapshot an empty store prefix";
        return false;
    }
    if (preview_detail::same_path(snapshot_root, impl_->root)) {
        *error = "prefix snapshot root must differ from the live store root";
        return false;
    }
    if (!impl_->records || !impl_->chunks || !impl_->search || !impl_->segment) {
        *error = "store files are not open for prefix snapshot";
        return false;
    }

    impl_->records.flush();
    impl_->chunks.flush();
    impl_->search.flush();
    impl_->segment.flush();
    if (!impl_->records || !impl_->chunks || !impl_->search || !impl_->segment) {
        *error = "failed to flush live store before prefix snapshot";
        return false;
    }

    std::error_code filesystem_error;
    std::filesystem::remove_all(snapshot_root, filesystem_error);
    filesystem_error.clear();
    std::filesystem::create_directories(snapshot_root / "segments", filesystem_error);
    if (filesystem_error) {
        *error = "cannot create prefix snapshot root: " + filesystem_error.message();
        return false;
    }

    std::uint64_t records_bytes = 0U;
    std::uint64_t chunks_bytes = 0U;
    std::uint64_t search_bytes = 0U;
    if (!preview_detail::multiply_exact(
            impl_->record_count,
            kRecordDescriptorBytes,
            &records_bytes,
            error,
            "record index") ||
        !preview_detail::multiply_exact(
            impl_->chunk_count,
            kChunkDescriptorBytes,
            &chunks_bytes,
            error,
            "chunk index") ||
        !preview_detail::multiply_exact(
            impl_->search_blocks,
            kBigramSignatureBytes,
            &search_bytes,
            error,
            "search index")) {
        return false;
    }

    if (!preview_detail::copy_exact_bounded(
            impl_->root / "records.idx",
            snapshot_root / "records.idx",
            records_bytes,
            error,
            "record index") ||
        !preview_detail::copy_exact_bounded(
            impl_->root / "chunks.idx",
            snapshot_root / "chunks.idx",
            chunks_bytes,
            error,
            "chunk index") ||
        !preview_detail::copy_exact_bounded(
            impl_->root / "search.bgm",
            snapshot_root / "search.bgm",
            search_bytes,
            error,
            "search index")) {
        return false;
    }

    std::uint64_t snapshot_search_blocks = impl_->search_blocks;
    if (impl_->records_in_block != 0U) {
        if (!preview_detail::append_partial_search_signature(
                snapshot_root / "search.bgm",
                impl_->signature,
                error)) {
            return false;
        }
        ++snapshot_search_blocks;
    }

    const std::uint64_t segment_count =
        static_cast<std::uint64_t>(impl_->segment_id) + 1U;
    std::vector<GenerationSegmentInventory> generation_segments;
    generation_segments.reserve(static_cast<std::size_t>(segment_count));
    for (std::uint64_t segment = 0U; segment < segment_count; ++segment) {
        const auto segment_id = static_cast<std::uint32_t>(segment);
        const auto source_path = segment_path(impl_->root, segment_id);
        std::error_code size_error;
        const std::uintmax_t raw_size = std::filesystem::file_size(source_path, size_error);
        if (size_error || raw_size > std::numeric_limits<std::uint64_t>::max()) {
            *error = "cannot size live segment for prefix snapshot";
            return false;
        }
        const std::uint64_t byte_length = static_cast<std::uint64_t>(raw_size);
        if (!preview_detail::copy_exact_bounded(
                source_path,
                segment_path(snapshot_root, segment_id),
                byte_length,
                error,
                "segment")) {
            return false;
        }
        generation_segments.push_back(
            GenerationSegmentInventory{segment_id, byte_length});
    }

    const CorpusMetadata metadata{
        impl_->logical_bytes,
        impl_->record_count,
        impl_->record_count,
        0U,
        0U,
        impl_->largest_record};
    const auto digest = impl_->sha.finish();
    std::vector<std::byte> manifest;
    manifest.reserve(kManifestBytes);
    for (const char character : kManifestMagic) {
        manifest.push_back(static_cast<std::byte>(character));
    }
    append_le(manifest, kFormatVersion);
    append_le(manifest, std::uint32_t{kManifestBytes});
    append_le(manifest, impl_->config.segment_bytes);
    append_le(manifest, impl_->config.records_per_search_block);
    append_le(manifest, std::uint32_t{0U});
    append_le(manifest, metadata.logical_utf8_bytes);
    append_le(manifest, metadata.logical_records);
    append_le(manifest, metadata.logical_nodes);
    append_le(manifest, metadata.style_runs);
    append_le(manifest, metadata.resource_references);
    append_le(manifest, metadata.largest_record_bytes);
    append_le(manifest, segment_count);
    append_le(manifest, impl_->chunk_count);
    append_le(manifest, snapshot_search_blocks);
    for (const std::uint8_t byte : digest) {
        manifest.push_back(static_cast<std::byte>(byte));
    }
    while (manifest.size() + sizeof(std::uint32_t) < kManifestBytes) {
        manifest.push_back(std::byte{0});
    }
    const std::uint32_t manifest_crc = crc32_update(0U, manifest);
    append_le(manifest, manifest_crc);
    if (manifest.size() != kManifestBytes) {
        *error = "internal prefix snapshot manifest size mismatch";
        return false;
    }

    if (!publish_store_generation(
            snapshot_root,
            1U,
            manifest,
            digest,
            generation_segments,
            GenerationPublicationCut::none,
            error)) {
        return false;
    }
    std::string compatibility_error;
    (void)publish_legacy_store_manifest(
        snapshot_root,
        manifest,
        &compatibility_error);

    if (stats != nullptr) {
        stats->corpus = metadata;
        stats->segment_count = segment_count;
        stats->chunk_count = impl_->chunk_count;
        stats->search_block_count = snapshot_search_blocks;
        stats->payload_sha256 = hex_digest(digest);
        stats->physical_bytes = directory_size(snapshot_root);
    }
    return true;
}

bool import_zmdoc_corpus_progressive(
    const std::filesystem::path& corpus_path,
    const std::filesystem::path& store_root,
    const std::filesystem::path& preview_root,
    ProgressiveImportConfig config,
    const ProgressivePreviewCallback& on_preview,
    StoreStats* final_stats,
    std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (config.preview_records == 0U || !on_preview) {
        *error = "progressive import requires a non-zero preview threshold and callback";
        return false;
    }
    if (preview_detail::same_path(store_root, preview_root)) {
        *error = "progressive preview root must differ from the final store root";
        return false;
    }

    std::ifstream corpus(corpus_path, std::ios::binary);
    if (!corpus) {
        *error = "cannot open ZMDOC corpus";
        return false;
    }
    CorpusMetadata final_metadata{};
    std::uint64_t total_records = 0U;
    if (!preview_detail::read_progressive_header(
            corpus,
            &final_metadata,
            &total_records,
            error)) {
        return false;
    }
    if (total_records <= config.preview_records) {
        *error = "preview threshold must be below the corpus record count";
        return false;
    }

    StoreWriter writer(store_root, config.store);
    bool preview_published = false;
    for (std::uint64_t index = 0U; index < total_records; ++index) {
        if (!preview_detail::append_progressive_record(corpus, &writer, error)) {
            return false;
        }
        const std::uint64_t imported_records = index + 1U;
        if (!preview_published && imported_records == config.preview_records) {
            StoreStats preview_store{};
            if (!writer.snapshot_prefix(
                    preview_root,
                    &preview_store,
                    error)) {
                return false;
            }
            ArenaStats preview_arena{};
            if (!build_compact_arena(
                    preview_root,
                    config.preview_arena,
                    &preview_arena,
                    error)) {
                return false;
            }
            ProgressivePreviewInfo info;
            info.root = preview_root;
            info.store = preview_store;
            info.arena = preview_arena;
            info.total_records = total_records;
            info.remaining_records = total_records - imported_records;
            error->clear();
            if (!on_preview(info, error)) {
                if (error->empty()) {
                    *error = "progressive preview callback rejected the preview";
                }
                return false;
            }
            preview_published = true;
        }
    }

    if (!preview_published) {
        *error = "progressive preview threshold was never reached";
        return false;
    }
    if (corpus.peek() != std::char_traits<char>::eof()) {
        *error = "ZMDOC corpus has trailing bytes";
        return false;
    }
    return writer.finalize(final_metadata, final_stats, error);
}

} // namespace zevryon::massivedoc
