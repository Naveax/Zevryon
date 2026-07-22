namespace zevryon::massivedoc {

bool LayoutCheckpointIndex::open(
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    LayoutCheckpointConfig config,
    std::string* error) {
    RecordInfo info;
    if (!read_record_info(store_root, record_index, &info, error)) {
        return false;
    }
    return open(
        store_root,
        record_index,
        info.logical_id,
        info.source_bytes,
        config,
        error);
}

} // namespace zevryon::massivedoc
