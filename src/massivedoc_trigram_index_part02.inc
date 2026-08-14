        if (impl_->previous_count >= 2U) {
            const std::uint32_t trigram = make_trigram(
                impl_->previous[0], impl_->previous[1], current);
            const auto encoded = serialize_spool(SpoolEntry{trigram, impl_->current_block});
            if (!write_all(impl_->spool, encoded)) {
                *error = "failed to append trigram spool entry";
                return false;
            }
            bloom_add(&impl_->current_bloom, trigram);
            ++impl_->spool_entries;
        }
        if (impl_->previous_count == 0U) {
            impl_->previous[0] = current;
            impl_->previous_count = 1U;
        } else if (impl_->previous_count == 1U) {
            impl_->previous[1] = current;
            impl_->previous_count = 2U;
        } else {
            impl_->previous[0] = impl_->previous[1];
            impl_->previous[1] = current;
        }
    }
    return true;
}

bool TrigramIndexWriter::end_record(std::string* error) {
    if (impl_ == nullptr || !impl_->in_record || impl_->finished) {
        *error = "trigram writer end_record requires an active record";
        return false;
    }
    impl_->in_record = false;
    impl_->previous_count = 0U;
    return true;
}

bool TrigramIndexWriter::finish(
    std::uint64_t total_blocks,
    TrigramIndexStats* stats,
    const TrigramCancellationCheck& cancelled,
    std::string* error) {
    if (stats != nullptr) {
        *stats = {};
    }
    if (impl_ == nullptr || impl_->finished || impl_->in_record) {
        *error = "trigram writer cannot finish in its current state";
        return false;
    }
    const std::uint64_t observed_blocks = impl_->have_block ? impl_->current_block + 1U : 0U;
    if (observed_blocks != total_blocks) {
        *error = "trigram block count does not match observed records";
        return false;
    }
    if (!impl_->flush_bloom(error)) {
        return false;
    }
    impl_->spool.flush();
    impl_->bloom.flush();
    if (!impl_->spool || !impl_->bloom) {
        *error = "failed to flush trigram temporary files";
        return false;
    }
    impl_->spool.close();
    impl_->bloom.close();

    const std::size_t max_entries = std::max<std::size_t>(
        1U,
        impl_->config.sort_memory_bytes / sizeof(SpoolEntry));
    std::vector<std::filesystem::path> runs;
    {
        std::ifstream input(impl_->spool_path, std::ios::binary);
        if (!input) {
            *error = "cannot reopen trigram spool";
            return false;
        }
        std::uint64_t run_number = 0U;
        for (;;) {
            if (cancellation_requested(cancelled)) {
                *error = "trigram index build cancelled";
                return false;
            }
            std::vector<SpoolEntry> entries;
            entries.reserve(max_entries);
            while (entries.size() < max_entries) {
                SpoolEntry entry;
                bool eof = false;
                if (!read_spool_entry(input, &entry, &eof, error)) {
                    return false;
                }
                if (eof) {
                    break;
                }
                entries.push_back(entry);
            }
            if (entries.empty()) {
                break;
            }
            if (stats != nullptr) {
                stats->peak_sort_entries = std::max(stats->peak_sort_entries, entries.size());
            }
            std::sort(entries.begin(), entries.end(), entry_less);
            entries.erase(
                std::unique(entries.begin(), entries.end(), entry_equal),
                entries.end());
            const auto path = impl_->temp_root / ("run-0-" + std::to_string(run_number++) + ".bin");
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) {
                *error = "cannot create trigram sort run";
                return false;
            }
            for (const auto& entry : entries) {
                if (!write_spool_entry(output, entry, error)) {
                    return false;
                }
            }
            output.flush();
            if (!output) {
                *error = "failed to flush trigram sort run";
                return false;
            }
            runs.push_back(path);
            if (input.eof()) {
                break;
            }
        }
    }

    std::uint64_t pass = 0U;
    while (runs.size() > 1U) {
        ++pass;
        std::vector<std::filesystem::path> next;
        for (std::size_t begin = 0U; begin < runs.size(); begin += impl_->config.merge_fan_in) {
            const std::size_t end = std::min(runs.size(), begin + impl_->config.merge_fan_in);
            std::vector<std::filesystem::path> group(
                runs.begin() + static_cast<std::ptrdiff_t>(begin),
                runs.begin() + static_cast<std::ptrdiff_t>(end));
            const auto output = impl_->temp_root /
                ("run-" + std::to_string(pass) + "-" + std::to_string(next.size()) + ".bin");
            if (!merge_run_group(group, output, cancelled, error)) {
                return false;
            }
            next.push_back(output);
        }
        for (const auto& path : runs) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        runs = std::move(next);
    }

    const auto sorted_path = impl_->temp_root / "sorted.bin";
    if (runs.empty()) {
        std::ofstream empty(sorted_path, std::ios::binary | std::ios::trunc);
        if (!empty) {
            *error = "cannot create empty trigram sorted run";
            return false;
        }
    } else {
        std::error_code fs_error;
        std::filesystem::rename(runs.front(), sorted_path, fs_error);
        if (fs_error) {
            *error = "cannot finalize trigram sorted run: " + fs_error.message();
            return false;
        }
    }

    const auto directory_path = impl_->temp_root / "directory.bin";
    const auto postings_path = impl_->temp_root / "postings.bin";
    std::ifstream sorted(sorted_path, std::ios::binary);
    std::ofstream directory(directory_path, std::ios::binary | std::ios::trunc);
    std::ofstream postings(postings_path, std::ios::binary | std::ios::trunc);
    if (!sorted || !directory || !postings) {
        *error = "cannot open trigram finalization streams";
        return false;
    }

    std::uint64_t postings_offset = 0U;
    std::uint64_t posting_pairs = 0U;
    std::uint64_t distinct = 0U;
    std::optional<std::uint32_t> current_trigram;
    std::uint64_t group_offset = 0U;
    std::uint64_t group_bytes = 0U;
    std::uint64_t group_count = 0U;
    std::uint64_t previous_block = 0U;
    bool have_previous_block = false;

    auto flush_group = [&]() -> bool {
        if (!current_trigram) {
            return true;
        }
        const DirectoryEntry entry{
            *current_trigram,
            group_offset,
            group_bytes,
            group_count,
        };
        const auto bytes = serialize_directory(entry);
        if (!write_all(directory, bytes)) {
            *error = "failed to write trigram directory entry";
            return false;
        }
        ++distinct;
        return true;
    };

    for (;;) {
        if ((posting_pairs & 0xfffU) == 0U && cancellation_requested(cancelled)) {
            *error = "trigram index build cancelled";
            return false;
        }
        SpoolEntry entry;
        bool eof = false;
        if (!read_spool_entry(sorted, &entry, &eof, error)) {
            return false;
        }
        if (eof) {
            break;
        }
        if (!current_trigram || *current_trigram != entry.trigram) {
            if (!flush_group()) {
                return false;
