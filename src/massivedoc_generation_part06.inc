namespace zevryon::massivedoc {
namespace {

constexpr std::uint32_t kMaximumRetainedCompactionGenerations = 64U;

struct RetainedCompactionGeneration {
    std::uint64_t generation{0U};
    JournalFingerprint fingerprint;
};

bool generation_quarantine_stale_manifest(
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
        store_root,
        path.filename().string() + ".stale");
    std::error_code rename_error;
    std::filesystem::rename(path, destination, rename_error);
    if (rename_error) {
        *error = "cannot quarantine stale generation manifest: " +
                 rename_error.message();
        return false;
    }
    if (!generation_durable_sync_directory(path.parent_path(), error)) {
        return false;
    }
    return generation_durable_sync_directory(destination.parent_path(), error);
}

bool generation_segments_equal(
    std::span<const GenerationSegmentInventory> left,
    std::span<const GenerationSegmentInventory> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].segment_id != right[index].segment_id ||
            left[index].byte_length != right[index].byte_length) {
            return false;
        }
    }
    return true;
}

bool generation_is_retained(
    std::uint64_t generation,
    std::span<const RetainedCompactionGeneration> retained) noexcept {
    return std::any_of(
        retained.begin(),
        retained.end(),
        [generation](const RetainedCompactionGeneration& candidate) {
            return candidate.generation == generation;
        });
}

std::optional<std::uint64_t> generation_number_from_manifest_name(
    const std::filesystem::path& path) {
    const std::string filename = path.filename().string();
    constexpr std::string_view prefix = "generation-";
    constexpr std::string_view suffix = ".zmg";
    if (filename.size() != prefix.size() + kGenerationDigits + suffix.size() ||
        filename.compare(0U, prefix.size(), prefix) != 0 ||
        filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::nullopt;
    }
    const std::string_view digits(
        filename.data() + static_cast<std::ptrdiff_t>(prefix.size()),
        kGenerationDigits);
    std::uint64_t value = 0U;
    for (const char character : digits) {
        std::uint8_t nibble = 0U;
        if (character >= '0' && character <= '9') {
            nibble = static_cast<std::uint8_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            nibble = static_cast<std::uint8_t>(character - 'a' + 10);
        } else {
            return std::nullopt;
        }
        if (value > (std::numeric_limits<std::uint64_t>::max() >> 4U)) {
            return std::nullopt;
        }
        value = (value << 4U) | nibble;
    }
    return value == 0U ? std::nullopt : std::optional<std::uint64_t>(value);
}

} // namespace

bool compact_store_generation_metadata(
    const std::filesystem::path& store_root,
    GenerationCompactionConfig config,
    GenerationCompactionCut cut,
    GenerationCompactionResult* result,
    std::string* error) {
    if (result == nullptr || error == nullptr) {
        return false;
    }
    *result = {};
    error->clear();
    if (config.retain_committed_generations == 0U ||
        config.retain_committed_generations > kMaximumRetainedCompactionGenerations) {
        *error = "generation compaction retention is outside the supported bounded range";
        return false;
    }

    GenerationRecovery authority;
    if (!recover_store_generation(store_root, &authority, error)) {
        return false;
    }
    if (!authority.protocol_present || !authority.found) {
        *error = "cannot compact generation metadata without committed authority";
        return false;
    }
    result->authority_generation = authority.generation;

    std::map<std::uint64_t, JournalGeneration> generations;
    bool tail_quarantined = false;
    if (!generation_load_journal(
            store_root,
            &generations,
            &tail_quarantined,
            error)) {
        return false;
    }
    static_cast<void>(tail_quarantined);

    const auto journal = store_generation_journal_path(store_root);
    std::error_code size_error;
    result->journal_bytes_before = std::filesystem::file_size(journal, size_error);
    if (size_error) {
        *error = "cannot size generation journal before compaction";
        return false;
    }

    std::vector<RetainedCompactionGeneration> retained_descending;
    retained_descending.reserve(config.retain_committed_generations);
    for (auto iterator = generations.rbegin();
         iterator != generations.rend() &&
         retained_descending.size() < config.retain_committed_generations;
         ++iterator) {
        const JournalGeneration& state = iterator->second;
        if (!state.prepare || !state.commit || *state.prepare != *state.commit) {
            continue;
        }
        const auto path = store_generation_path(store_root, iterator->first);
        std::error_code exists_error;
        const bool exists = std::filesystem::exists(path, exists_error);
        if (exists_error) {
            *error = "cannot inspect generation manifest during compaction";
            return false;
        }
        if (!exists) {
            continue;
        }
        ParsedGeneration parsed;
        std::string parse_error;
        if (!generation_parse_manifest_file(
                store_root,
                path,
                iterator->first,
                *state.commit,
                &parsed,
                &parse_error)) {
            continue;
        }
        retained_descending.push_back(
            RetainedCompactionGeneration{iterator->first, *state.commit});
    }
    if (retained_descending.empty() ||
        retained_descending.front().generation != authority.generation) {
        *error = "generation compaction could not retain current authority";
        return false;
    }

    std::vector<RetainedCompactionGeneration> retained(
        retained_descending.rbegin(),
        retained_descending.rend());
    result->retained_committed_generations =
        static_cast<std::uint64_t>(retained.size());

    if (retained.size() >
        std::numeric_limits<std::size_t>::max() / (2U * kJournalRecordBytes)) {
        *error = "generation compaction journal size overflow";
        return false;
    }
    std::vector<std::byte> compacted_journal;
    compacted_journal.reserve(retained.size() * 2U * kJournalRecordBytes);
    for (const auto& entry : retained) {
        const auto prepare = generation_serialize_journal_record(
            entry.generation,
            JournalState::prepare,
            entry.fingerprint);
        const auto commit = generation_serialize_journal_record(
            entry.generation,
            JournalState::commit,
            entry.fingerprint);
        compacted_journal.insert(
            compacted_journal.end(),
            prepare.begin(),
            prepare.end());
        compacted_journal.insert(
            compacted_journal.end(),
            commit.begin(),
            commit.end());
    }
    result->journal_bytes_after =
        static_cast<std::uint64_t>(compacted_journal.size());

    const auto compact_temp =
        std::filesystem::path(journal.string() + ".compact.tmp");
    if (!generation_remove_stale_temp(compact_temp, error)) {
        return false;
    }
    if (!generation_write_durable_temp(compact_temp, compacted_journal, error)) {
        return false;
    }
    if (cut == GenerationCompactionCut::after_journal_temp) {
        return true;
    }
    if (!generation_replace_file(compact_temp, journal, error) ||
        !generation_durable_flush_file(journal, error)) {
        return false;
    }
    if (cut == GenerationCompactionCut::after_journal_replace) {
        return true;
    }

    std::error_code iteration_error;
    for (std::filesystem::directory_iterator iterator(
             generation_root(store_root),
             iteration_error),
         end;
         iterator != end && !iteration_error;
         iterator.increment(iteration_error)) {
        std::error_code type_error;
        if (!iterator->is_regular_file(type_error)) {
            if (type_error) {
                *error = "cannot inspect generation file during compaction";
                return false;
            }
            continue;
        }
        const auto generation = generation_number_from_manifest_name(iterator->path());
        if (!generation || generation_is_retained(*generation, retained)) {
            continue;
        }
        if (!generation_quarantine_stale_manifest(
                store_root,
                iterator->path(),
                error)) {
            return false;
        }
        ++result->quarantined_stale_manifests;
    }
    if (iteration_error) {
        *error = "cannot enumerate generation manifests during compaction: " +
                 iteration_error.message();
        return false;
    }

    GenerationRecovery verified;
    if (!recover_store_generation(store_root, &verified, error)) {
        return false;
    }
    if (!verified.found || verified.generation != authority.generation ||
        verified.source_identity != authority.source_identity ||
        verified.authority_manifest != authority.authority_manifest ||
        !generation_segments_equal(verified.segments, authority.segments)) {
        *error = "generation compaction changed committed authority";
        return false;
    }
    return true;
}

} // namespace zevryon::massivedoc
