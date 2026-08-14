bool generation_serialize_manifest(
    std::uint64_t generation,
    std::span<const std::byte> authority_manifest,
    const std::array<std::uint8_t, 32>& source_identity,
    std::span<const GenerationSegmentInventory> segments,
    std::vector<std::byte>* bytes,
    std::uint32_t* manifest_crc,
    std::string* error) {
    if (generation == 0U || bytes == nullptr || manifest_crc == nullptr) {
        *error = "invalid generation serialization request";
        return false;
    }
    if (authority_manifest.empty() ||
        authority_manifest.size() > static_cast<std::size_t>(kMaxAuthorityManifestBytes)) {
        *error = "generation authority manifest size is unsupported";
        return false;
    }
    if (segments.size() > static_cast<std::size_t>(kMaxGenerationSegments)) {
        *error = "generation segment inventory is too large";
        return false;
    }
    if (segments.size() >
        (std::numeric_limits<std::size_t>::max() - kGenerationHeaderBytes -
         authority_manifest.size() - kGenerationTrailerBytes) /
            kGenerationInventoryBytes) {
        *error = "generation manifest size overflow";
        return false;
    }

    const std::uint64_t authority_bytes =
        static_cast<std::uint64_t>(authority_manifest.size());
    const std::uint64_t inventory_count =
        static_cast<std::uint64_t>(segments.size());
    bytes->clear();
    bytes->reserve(
        kGenerationHeaderBytes + authority_manifest.size() +
        segments.size() * kGenerationInventoryBytes + kGenerationTrailerBytes);
    for (const char value : kGenerationMagic) {
        bytes->push_back(
            static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    generation_append_le(bytes, kGenerationVersion);
    generation_append_le(bytes, static_cast<std::uint32_t>(kGenerationHeaderBytes));
    generation_append_le(bytes, generation);
    generation_append_le(bytes, authority_bytes);
    generation_append_le(bytes, inventory_count);
    generation_append_le(bytes, generation_crc32_update(0U, authority_manifest));
    generation_append_le(bytes, std::uint32_t{0U});
    for (const std::uint8_t value : source_identity) {
        bytes->push_back(static_cast<std::byte>(value));
    }
    bytes->insert(bytes->end(), authority_manifest.begin(), authority_manifest.end());
    for (const auto& segment : segments) {
        generation_append_le(bytes, segment.segment_id);
        generation_append_le(bytes, std::uint32_t{0U});
        generation_append_le(bytes, segment.byte_length);
    }
    const std::uint32_t crc = generation_crc32_update(0U, *bytes);
    generation_append_le(bytes, crc);
    *manifest_crc = crc;
    return true;
}

std::vector<std::byte> generation_serialize_journal_record(
    std::uint64_t generation,
    JournalState state,
    JournalFingerprint fingerprint) {
    std::vector<std::byte> bytes;
    bytes.reserve(kJournalRecordBytes);
    for (const char value : kJournalMagic) {
        bytes.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    generation_append_le(&bytes, kJournalVersion);
    generation_append_le(&bytes, static_cast<std::uint32_t>(kJournalRecordBytes));
    generation_append_le(&bytes, generation);
    generation_append_le(&bytes, static_cast<std::uint32_t>(state));
    generation_append_le(&bytes, fingerprint.manifest_crc);
    generation_append_le(&bytes, fingerprint.manifest_bytes);
    generation_append_le(&bytes, std::uint64_t{0U});
    generation_append_le(&bytes, generation_crc32_update(0U, bytes));
    return bytes;
}

bool generation_append_journal_record(
    const std::filesystem::path& store_root,
    std::uint64_t generation,
    JournalState state,
    JournalFingerprint fingerprint,
    std::string* error) {
    const auto journal = store_generation_journal_path(store_root);
    const auto bytes = generation_serialize_journal_record(generation, state, fingerprint);
    std::ofstream stream(journal, std::ios::binary | std::ios::app);
    if (!stream || !generation_write_all(stream, bytes)) {
        *error = "cannot append generation journal";
        return false;
    }
    stream.flush();
    if (!stream) {
        *error = "cannot flush generation journal";
        return false;
    }
    stream.close();
    if (stream.fail()) {
        *error = "cannot close generation journal";
        return false;
    }
    if (!generation_durable_flush_file(journal, error)) {
        return false;
    }
    return generation_durable_sync_directory(journal.parent_path(), error);
}

bool generation_parse_journal_record(
    std::span<const std::byte> bytes,
    std::uint64_t* generation,
    JournalState* state,
    JournalFingerprint* fingerprint) {
    if (bytes.size() != kJournalRecordBytes || generation == nullptr ||
        state == nullptr || fingerprint == nullptr) {
        return false;
    }
    for (std::size_t index = 0U; index < kJournalMagic.size(); ++index) {
        if (std::to_integer<std::uint8_t>(bytes[index]) !=
            static_cast<std::uint8_t>(
                static_cast<unsigned char>(kJournalMagic[index]))) {
            return false;
        }
    }
    const auto version = generation_read_le<std::uint32_t>(bytes, 8U);
    const auto record_bytes = generation_read_le<std::uint32_t>(bytes, 12U);
    const auto parsed_generation = generation_read_le<std::uint64_t>(bytes, 16U);
    const auto raw_state = generation_read_le<std::uint32_t>(bytes, 24U);
    const auto manifest_crc = generation_read_le<std::uint32_t>(bytes, 28U);
    const auto manifest_bytes = generation_read_le<std::uint64_t>(bytes, 32U);
    const auto reserved = generation_read_le<std::uint64_t>(bytes, 40U);
    const auto expected_crc = generation_read_le<std::uint32_t>(bytes, kJournalPayloadBytes);
    if (!version || !record_bytes || !parsed_generation || !raw_state ||
        !manifest_crc || !manifest_bytes || !reserved || !expected_crc ||
        *version != kJournalVersion ||
        *record_bytes != static_cast<std::uint32_t>(kJournalRecordBytes) ||
        *parsed_generation == 0U || *reserved != 0U) {
        return false;
    }
    if (*raw_state != static_cast<std::uint32_t>(JournalState::prepare) &&
        *raw_state != static_cast<std::uint32_t>(JournalState::commit)) {
        return false;
    }
    if (*manifest_bytes <
            static_cast<std::uint64_t>(
                kGenerationHeaderBytes + kGenerationTrailerBytes) ||
        *manifest_bytes >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    const std::uint32_t actual_crc =
        generation_crc32_update(0U, bytes.first(kJournalPayloadBytes));
    if (*expected_crc != actual_crc) {
        return false;
    }
    *generation = *parsed_generation;
    *state = static_cast<JournalState>(*raw_state);
    *fingerprint = JournalFingerprint{*manifest_crc, *manifest_bytes};
    return true;
}

std::filesystem::path generation_unique_quarantine_path(
    const std::filesystem::path& store_root,
    std::string_view base_name) {
    const auto root = generation_quarantine_root(store_root);
    std::filesystem::path candidate = root / std::string(base_name);
    std::error_code exists_error;
    if (!std::filesystem::exists(candidate, exists_error) && !exists_error) {
        return candidate;
    }
    for (std::uint32_t suffix = 1U; suffix != 0U; ++suffix) {
        std::ostringstream name;
        name << base_name << "." << suffix;
        candidate = root / name.str();
        exists_error.clear();
        if (!std::filesystem::exists(candidate, exists_error) && !exists_error) {
            return candidate;
        }
    }
    return root / (std::string(base_name) + ".overflow");
}

bool generation_copy_file_range(
    const std::filesystem::path& source,
    std::uint64_t offset,
    std::uint64_t length,
    const std::filesystem::path& destination,
    std::string* error) {
    std::ifstream input(source, std::ios::binary);
    if (!input ||
        offset >
            static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        *error = "cannot open generation journal range";
        return false;
    }
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        *error = "cannot seek generation journal range";
        return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
        *error = "cannot create generation quarantine file";
        return false;
    }
    std::vector<std::byte> buffer(kCopyWindowBytes);
    std::uint64_t remaining = length;
    while (remaining != 0U) {
        const std::size_t amount = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        if (!generation_read_exact(input, std::span<std::byte>(buffer.data(), amount)) ||
            !generation_write_all(output, std::span<const std::byte>(buffer.data(), amount))) {
            *error = "cannot copy generation journal range";
            return false;
        }
        remaining -= static_cast<std::uint64_t>(amount);
    }
    output.flush();
    if (!output) {
        *error = "cannot flush generation quarantine file";
        return false;
    }
    output.close();
    if (output.fail()) {
        *error = "cannot close generation quarantine file";
        return false;
    }
    return generation_durable_flush_file(destination, error);
}

bool generation_quarantine_and_repair_journal_tail(
    const std::filesystem::path& store_root,
    std::uint64_t valid_prefix,
    std::uint64_t total_bytes,
    std::string* error) {
    if (valid_prefix >= total_bytes) {
        return true;
    }
    if (!generation_ensure_directory(
            generation_quarantine_root(store_root),
            "cannot create generation quarantine directory",
            error)) {
        return false;
    }
    std::ostringstream quarantine_name;
    quarantine_name << "journal-tail-" << std::hex << std::setfill('0')
                    << std::setw(16) << valid_prefix << ".bin";
    const auto quarantined =
        generation_unique_quarantine_path(store_root, quarantine_name.str());
    const auto quarantined_temp =
        std::filesystem::path(quarantined.string() + ".tmp");
    if (!generation_remove_stale_temp(quarantined_temp, error)) {
        return false;
    }
    if (!generation_copy_file_range(
            store_generation_journal_path(store_root),
            valid_prefix,
            total_bytes - valid_prefix,
            quarantined_temp,
            error)) {
        return false;
    }
    if (!generation_publish_no_replace(quarantined_temp, quarantined, error)) {
        return false;
    }
    if (!generation_durable_sync_directory(generation_quarantine_root(store_root), error)) {
        return false;
    }

    const auto journal = store_generation_journal_path(store_root);
    const auto repair_temp =
        std::filesystem::path(journal.string() + ".repair.tmp");
    if (!generation_remove_stale_temp(repair_temp, error)) {
        return false;
    }
    if (!generation_copy_file_range(journal, 0U, valid_prefix, repair_temp, error)) {
        return false;
    }
    if (!generation_replace_file(repair_temp, journal, error)) {
        return false;
    }
    return generation_durable_flush_file(journal, error);
}

bool generation_quarantine_manifest(
    const std::filesystem::path& store_root,
    const std::filesystem::path& path,
    std::string* error) {
    if (!generation_ensure_directory(
            generation_quarantine_root(store_root),
            "cannot create generation quarantine directory",
            error)) {
        return false;
    }
    const auto destination = generation_unique_quarantine_path(
        store_root, path.filename().string() + ".corrupt");
    std::error_code rename_error;
    std::filesystem::rename(path, destination, rename_error);
    if (rename_error) {
        *error = "cannot quarantine corrupt generation manifest: " +
                 rename_error.message();
        return false;
    }
    if (!generation_durable_sync_directory(path.parent_path(), error)) {
        return false;
    }
    return generation_durable_sync_directory(destination.parent_path(), error);
}

#include "massivedoc_generation_part03.inc"
