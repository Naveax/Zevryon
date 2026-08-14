std::filesystem::path store_generation_path(
    const std::filesystem::path& store_root,
    std::uint64_t generation) {
    return generation_root(store_root) / generation_file_name(generation, false);
}

std::filesystem::path store_generation_journal_path(
    const std::filesystem::path& store_root) {
    return generation_root(store_root) / "generation.journal";
}

bool publish_store_generation(
    const std::filesystem::path& store_root,
    std::uint64_t generation,
    std::span<const std::byte> authority_manifest,
    const std::array<std::uint8_t, 32>& source_identity,
    std::span<const GenerationSegmentInventory> segments,
    GenerationPublicationCut cut,
    std::string* error) {
    if (error == nullptr || generation == 0U) {
        if (error != nullptr) {
            *error = "invalid generation publication request";
        }
        return false;
    }
    error->clear();
    if (!generation_ensure_directory(
            generation_root(store_root),
            "cannot create generation directory",
            error)) {
        return false;
    }
    if (!generation_validate_inventory(store_root, segments, error)) {
        return false;
    }

    GenerationRecovery current;
    if (!recover_store_generation(store_root, &current, error)) {
        return false;
    }
    if (current.found && generation <= current.generation) {
        *error = "generation publication is not newer than committed authority";
        return false;
    }

    std::vector<std::byte> generation_bytes;
    std::uint32_t manifest_crc = 0U;
    if (!generation_serialize_manifest(
            generation,
            authority_manifest,
            source_identity,
            segments,
            &generation_bytes,
            &manifest_crc,
            error)) {
        return false;
    }
    const JournalFingerprint fingerprint{
        manifest_crc, static_cast<std::uint64_t>(generation_bytes.size())};

    const auto final_path = store_generation_path(store_root, generation);
    const auto temporary_path = generation_temporary_path(store_root, generation);
    std::error_code exists_error;
    if (std::filesystem::exists(final_path, exists_error)) {
        if (exists_error) {
            *error = "cannot inspect generation final file";
        } else {
            *error = "generation final file already exists";
        }
        return false;
    }
    if (exists_error || !generation_remove_stale_temp(temporary_path, error)) {
        if (exists_error) {
            *error = "cannot inspect generation final file";
        }
        return false;
    }

    if (!generation_durable_flush_store_payload(store_root, segments, error)) {
        return false;
    }
    if (!generation_append_journal_record(
            store_root,
            generation,
            JournalState::prepare,
            fingerprint,
            error)) {
        return false;
    }
    if (cut == GenerationPublicationCut::after_prepare) {
        return true;
    }

    if (!generation_write_durable_temp(temporary_path, generation_bytes, error) ||
        !generation_publish_no_replace(temporary_path, final_path, error)) {
        return false;
    }
    if (cut == GenerationPublicationCut::after_manifest) {
        return true;
    }

    if (!generation_append_journal_record(
            store_root,
            generation,
            JournalState::commit,
            fingerprint,
            error)) {
        return false;
    }

    GenerationRecovery committed;
    if (!recover_store_generation(store_root, &committed, error)) {
        return false;
    }
    if (!committed.found || committed.generation != generation ||
        committed.source_identity != source_identity ||
        committed.authority_manifest.size() != authority_manifest.size() ||
        !std::equal(
            committed.authority_manifest.begin(),
            committed.authority_manifest.end(),
            authority_manifest.begin(),
            authority_manifest.end())) {
        *error = "committed generation verification mismatch";
        return false;
    }
    return true;
}

bool publish_legacy_store_manifest(
    const std::filesystem::path& store_root,
    std::span<const std::byte> authority_manifest,
    std::string* error) {
    if (error == nullptr || authority_manifest.empty()) {
        if (error != nullptr) {
            *error = "invalid legacy store manifest publication request";
        }
        return false;
    }
    error->clear();
    const auto final_path = store_root / "manifest.zmd";
    const auto temporary_path = store_root / "manifest.zmd.tmp";
    std::error_code exists_error;
    if (std::filesystem::exists(final_path, exists_error)) {
        if (exists_error) {
            *error = "cannot inspect legacy store manifest";
        } else {
            *error = "legacy store manifest already exists";
        }
        return false;
    }
    if (exists_error || !generation_remove_stale_temp(temporary_path, error)) {
        if (exists_error) {
            *error = "cannot inspect legacy store manifest";
        }
        return false;
    }
    if (!generation_write_durable_temp(temporary_path, authority_manifest, error) ||
        !generation_publish_no_replace(temporary_path, final_path, error)) {
        return false;
    }
    return generation_durable_sync_directory(store_root, error);
}

bool recover_store_generation(
    const std::filesystem::path& store_root,
    GenerationRecovery* recovery,
    std::string* error) {
    if (recovery == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    *recovery = GenerationRecovery{};

    const auto journal = store_generation_journal_path(store_root);
    std::error_code exists_error;
    const bool journal_exists = std::filesystem::exists(journal, exists_error);
    if (exists_error) {
        *error = "cannot inspect generation journal";
        return false;
    }
    if (!journal_exists) {
        return true;
    }
    recovery->protocol_present = true;

    std::map<std::uint64_t, JournalGeneration> generations;
    if (!generation_load_journal(
            store_root,
            &generations,
            &recovery->journal_tail_quarantined,
            error)) {
        return false;
    }

    for (auto iterator = generations.rbegin(); iterator != generations.rend();
         ++iterator) {
        const std::uint64_t generation = iterator->first;
        const JournalGeneration& state = iterator->second;
        if (!state.prepare || !state.commit || *state.prepare != *state.commit) {
            continue;
        }
        const auto path = store_generation_path(store_root, generation);
        std::error_code manifest_exists_error;
        const bool manifest_exists =
            std::filesystem::exists(path, manifest_exists_error);
        if (manifest_exists_error) {
            *error = "cannot inspect committed generation manifest";
            return false;
        }
        if (!manifest_exists) {
            continue;
        }

        ParsedGeneration parsed;
        std::string parse_error;
        if (!generation_parse_manifest_file(
                store_root,
                path,
                generation,
                *state.commit,
                &parsed,
                &parse_error)) {
            if (!generation_quarantine_manifest(store_root, path, error)) {
                return false;
            }
            ++recovery->quarantined_manifests;
            continue;
        }

        recovery->found = true;
        recovery->generation = parsed.generation;
        recovery->source_identity = parsed.source_identity;
        recovery->authority_manifest = std::move(parsed.authority_manifest);
        recovery->segments = std::move(parsed.segments);
        return true;
    }
    return true;
}

} // namespace zevryon::massivedoc
