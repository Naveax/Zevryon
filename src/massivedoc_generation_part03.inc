bool generation_parse_manifest_file(
    const std::filesystem::path& store_root,
    const std::filesystem::path& path,
    std::uint64_t expected_generation,
    JournalFingerprint expected_fingerprint,
    ParsedGeneration* parsed,
    std::string* error) {
    if (parsed == nullptr) {
        *error = "invalid generation parse destination";
        return false;
    }
    std::error_code size_error;
    const std::uint64_t file_bytes = std::filesystem::file_size(path, size_error);
    if (size_error || file_bytes != expected_fingerprint.manifest_bytes ||
        file_bytes >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        *error = "generation manifest size mismatch";
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *error = "cannot open generation manifest";
        return false;
    }
    std::array<std::byte, kGenerationHeaderBytes> header{};
    if (!generation_read_exact(stream, header)) {
        *error = "truncated generation manifest header";
        return false;
    }
    for (std::size_t index = 0U; index < kGenerationMagic.size(); ++index) {
        if (std::to_integer<std::uint8_t>(header[index]) !=
            static_cast<std::uint8_t>(
                static_cast<unsigned char>(kGenerationMagic[index]))) {
            *error = "invalid generation manifest magic";
            return false;
        }
    }
    const auto version = generation_read_le<std::uint32_t>(header, 8U);
    const auto header_bytes = generation_read_le<std::uint32_t>(header, 12U);
    const auto generation = generation_read_le<std::uint64_t>(header, 16U);
    const auto authority_bytes = generation_read_le<std::uint64_t>(header, 24U);
    const auto inventory_count = generation_read_le<std::uint64_t>(header, 32U);
    const auto authority_crc = generation_read_le<std::uint32_t>(header, 40U);
    const auto reserved = generation_read_le<std::uint32_t>(header, 44U);
    if (!version || !header_bytes || !generation || !authority_bytes ||
        !inventory_count || !authority_crc || !reserved ||
        *version != kGenerationVersion ||
        *header_bytes != static_cast<std::uint32_t>(kGenerationHeaderBytes) ||
        *generation != expected_generation || *reserved != 0U ||
        *authority_bytes == 0U ||
        *authority_bytes > kMaxAuthorityManifestBytes ||
        *inventory_count > kMaxGenerationSegments) {
        *error = "unsupported generation manifest header";
        return false;
    }
    if (*inventory_count >
        (std::numeric_limits<std::uint64_t>::max() -
         static_cast<std::uint64_t>(kGenerationHeaderBytes) - *authority_bytes -
         static_cast<std::uint64_t>(kGenerationTrailerBytes)) /
            static_cast<std::uint64_t>(kGenerationInventoryBytes)) {
        *error = "generation manifest inventory size overflow";
        return false;
    }
    const std::uint64_t expected_bytes =
        static_cast<std::uint64_t>(kGenerationHeaderBytes) + *authority_bytes +
        *inventory_count * static_cast<std::uint64_t>(kGenerationInventoryBytes) +
        static_cast<std::uint64_t>(kGenerationTrailerBytes);
    if (file_bytes != expected_bytes) {
        *error = "generation manifest layout size mismatch";
        return false;
    }

    parsed->generation = *generation;
    for (std::size_t index = 0U; index < parsed->source_identity.size(); ++index) {
        parsed->source_identity[index] =
            std::to_integer<std::uint8_t>(header[48U + index]);
    }
    parsed->authority_manifest.assign(
        static_cast<std::size_t>(*authority_bytes), std::byte{0});
    if (!generation_read_exact(stream, parsed->authority_manifest)) {
        *error = "truncated generation authority manifest";
        return false;
    }
    if (generation_crc32_update(0U, parsed->authority_manifest) != *authority_crc) {
        *error = "generation authority manifest CRC mismatch";
        return false;
    }

    parsed->segments.clear();
    parsed->segments.reserve(static_cast<std::size_t>(*inventory_count));
    std::array<std::byte, kGenerationInventoryBytes> inventory_bytes{};
    for (std::uint64_t index = 0U; index < *inventory_count; ++index) {
        if (!generation_read_exact(stream, inventory_bytes)) {
            *error = "truncated generation segment inventory";
            return false;
        }
        const auto segment_id = generation_read_le<std::uint32_t>(inventory_bytes, 0U);
        const auto inventory_reserved = generation_read_le<std::uint32_t>(inventory_bytes, 4U);
        const auto byte_length = generation_read_le<std::uint64_t>(inventory_bytes, 8U);
        if (!segment_id || !inventory_reserved || !byte_length ||
            *inventory_reserved != 0U ||
            index >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::uint32_t>::max()) ||
            *segment_id != static_cast<std::uint32_t>(index)) {
            *error = "invalid generation segment inventory";
            return false;
        }
        parsed->segments.push_back(
            GenerationSegmentInventory{*segment_id, *byte_length});
    }

    std::array<std::byte, kGenerationTrailerBytes> trailer{};
    if (!generation_read_exact(stream, trailer)) {
        *error = "truncated generation manifest trailer";
        return false;
    }
    const auto stored_crc = generation_read_le<std::uint32_t>(trailer, 0U);
    if (!stored_crc || *stored_crc != expected_fingerprint.manifest_crc) {
        *error = "generation manifest journal fingerprint mismatch";
        return false;
    }

    std::uint32_t manifest_crc = generation_crc32_update(0U, header);
    manifest_crc = generation_crc32_update(manifest_crc, parsed->authority_manifest);
    stream.clear();
    const std::uint64_t inventory_offset =
        static_cast<std::uint64_t>(kGenerationHeaderBytes) + *authority_bytes;
    if (inventory_offset >
        static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        *error = "generation manifest inventory offset overflow";
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(inventory_offset), std::ios::beg);
    if (!stream) {
        *error = "cannot seek generation segment inventory";
        return false;
    }
    for (std::uint64_t index = 0U; index < *inventory_count; ++index) {
        if (!generation_read_exact(stream, inventory_bytes)) {
            *error = "truncated generation segment inventory";
            return false;
        }
        manifest_crc = generation_crc32_update(manifest_crc, inventory_bytes);
    }
    if (manifest_crc != *stored_crc) {
        *error = "generation manifest CRC mismatch";
        return false;
    }
    if (!generation_validate_inventory(store_root, parsed->segments, error)) {
        return false;
    }
    return true;
}

#include "massivedoc_generation_part04.inc"
