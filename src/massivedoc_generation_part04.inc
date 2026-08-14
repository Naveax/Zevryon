bool generation_load_journal(
    const std::filesystem::path& store_root,
    std::map<std::uint64_t, JournalGeneration>* generations,
    bool* tail_quarantined,
    std::string* error) {
    if (generations == nullptr || tail_quarantined == nullptr) {
        *error = "invalid generation journal destination";
        return false;
    }
    generations->clear();
    *tail_quarantined = false;
    const auto journal = store_generation_journal_path(store_root);
    std::error_code size_error;
    const std::uint64_t total_bytes = std::filesystem::file_size(journal, size_error);
    if (size_error) {
        *error = "cannot size generation journal";
        return false;
    }
    std::ifstream stream(journal, std::ios::binary);
    if (!stream) {
        *error = "cannot open generation journal";
        return false;
    }

    std::uint64_t offset = 0U;
    std::array<std::byte, kJournalRecordBytes> bytes{};
    while (total_bytes - offset >=
           static_cast<std::uint64_t>(kJournalRecordBytes)) {
        if (!generation_read_exact(stream, bytes)) {
            break;
        }
        std::uint64_t generation = 0U;
        JournalState state = JournalState::prepare;
        JournalFingerprint fingerprint;
        if (!generation_parse_journal_record(bytes, &generation, &state, &fingerprint)) {
            break;
        }
        auto& entry = (*generations)[generation];
        if (state == JournalState::prepare) {
            if (entry.prepare && *entry.prepare != fingerprint) {
                break;
            }
            if (entry.commit) {
                break;
            }
            entry.prepare = fingerprint;
        } else {
            if (!entry.prepare || *entry.prepare != fingerprint ||
                (entry.commit && *entry.commit != fingerprint)) {
                break;
            }
            entry.commit = fingerprint;
        }
        offset += static_cast<std::uint64_t>(kJournalRecordBytes);
    }

    // Windows will not replace the journal while this read handle is still open.
    // Release it before quarantine/repair so the recovery transaction has the
    // same observable semantics on Windows and POSIX.
    stream.close();

    if (offset != total_bytes) {
        if (!generation_quarantine_and_repair_journal_tail(
                store_root, offset, total_bytes, error)) {
            return false;
        }
        *tail_quarantined = true;
    }
    return true;
}

} // namespace

#include "massivedoc_generation_part05.inc"
