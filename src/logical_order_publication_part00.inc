namespace {

bool serialize_logical_order_snapshot(
    std::uint64_t generation,
    std::span<const std::uint64_t> source_record_indices,
    std::vector<std::byte>* bytes,
    std::string* error) {
    if (bytes == nullptr || error == nullptr || generation == 0U) {
        if (error != nullptr) {
            *error = "invalid logical order publication request";
        }
        return false;
    }
    error->clear();
    if (source_record_indices.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
        *error = "logical order publication record count overflow";
        return false;
    }
    const std::uint64_t logical_records =
        static_cast<std::uint64_t>(source_record_indices.size());
    if (logical_records > std::numeric_limits<std::uint64_t>::max() /
                              static_cast<std::uint64_t>(kLogicalOrderEntryBytes)) {
        *error = "logical order publication payload overflow";
        return false;
    }
    const std::uint64_t payload_bytes =
        logical_records * static_cast<std::uint64_t>(kLogicalOrderEntryBytes);
    if (payload_bytes > static_cast<std::uint64_t>(
                            std::numeric_limits<std::size_t>::max() -
                            kLogicalOrderHeaderBytes - kLogicalOrderTrailerBytes)) {
        *error = "logical order publication exceeds addressable memory";
        return false;
    }

    bytes->clear();
    bytes->reserve(
        kLogicalOrderHeaderBytes + static_cast<std::size_t>(payload_bytes) +
        kLogicalOrderTrailerBytes);
    for (const char value : kLogicalOrderMagic) {
        bytes->push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    append_le(*bytes, kLogicalOrderVersion);
    append_le(*bytes, static_cast<std::uint32_t>(kLogicalOrderHeaderBytes));
    append_le(*bytes, logical_records);
    append_le(*bytes, generation);
    append_le(*bytes, payload_bytes);
    for (const std::uint64_t source_record_index : source_record_indices) {
        append_le(*bytes, source_record_index);
    }
    append_le(*bytes, logical_order_crc32_update(0U, *bytes));

    LogicalOrderSnapshot verified;
    std::string verify_error;
    if (!parse_logical_order_snapshot(*bytes, logical_records, &verified, &verify_error)) {
        *error = "cannot serialize logical order snapshot: " + verify_error;
        return false;
    }
    if (verified.generation != generation ||
        verified.source_record_indices.size() != source_record_indices.size() ||
        !std::equal(
            verified.source_record_indices.begin(),
            verified.source_record_indices.end(),
            source_record_indices.begin(),
            source_record_indices.end())) {
        *error = "logical order snapshot serialization verification mismatch";
        return false;
    }
    return true;
}

bool durable_flush_logical_order_file(
    const std::filesystem::path& path,
    std::string* error) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = "cannot open logical order temporary file for durable flush";
        return false;
    }
    const BOOL flushed = FlushFileBuffers(handle);
    const BOOL closed = CloseHandle(handle);
    if (flushed == 0 || closed == 0) {
        *error = "cannot durably flush logical order temporary file";
        return false;
    }
    return true;
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        *error = "cannot open logical order temporary file for durable flush";
        return false;
    }
    const int flush_result = ::fsync(descriptor);
    const int close_result = ::close(descriptor);
    if (flush_result != 0 || close_result != 0) {
        *error = "cannot durably flush logical order temporary file";
        return false;
    }
    return true;
#endif
}

bool publish_logical_order_temp_file(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& final_path,
    std::string* error) {
#if defined(_WIN32)
    if (MoveFileExW(
            temporary_path.c_str(),
            final_path.c_str(),
            MOVEFILE_WRITE_THROUGH) == 0) {
        *error = "cannot commit logical order generation";
        return false;
    }
    return true;
#else
    if (::link(temporary_path.c_str(), final_path.c_str()) != 0) {
        *error = "cannot commit logical order generation without replacement";
        return false;
    }
    const auto arena_root = final_path.parent_path();
    const int directory = ::open(
        arena_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        *error = "cannot open logical order arena for directory sync";
        return false;
    }
    const int sync_result = ::fsync(directory);
    const int close_result = ::close(directory);
    if (sync_result != 0 || close_result != 0) {
        *error = "cannot durably sync committed logical order generation";
        return false;
    }
    std::error_code cleanup_error;
    static_cast<void>(std::filesystem::remove(temporary_path, cleanup_error));
    return true;
#endif
}

} // namespace

bool publish_logical_order_snapshot(
    const std::filesystem::path& store_root,
    std::uint64_t generation,
    std::span<const std::uint64_t> source_record_indices,
    std::string* error) {
    if (error == nullptr || generation == 0U) {
        if (error != nullptr) {
            *error = "invalid logical order publication request";
        }
        return false;
    }
    error->clear();
    if (source_record_indices.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
        *error = "logical order publication record count overflow";
        return false;
    }
    const std::uint64_t logical_records =
        static_cast<std::uint64_t>(source_record_indices.size());

    LogicalOrderSnapshot current;
    if (!load_logical_order_snapshot(
            store_root, logical_records, &current, error)) {
        return false;
    }
    if (generation <= current.generation) {
        *error = "logical order publication generation is not newer";
        return false;
    }

    const auto final_path = logical_order_generation_path(store_root, generation);
    const auto temporary_path =
        logical_order_generation_temp_path(store_root, generation);
    std::error_code status_error;
    if (std::filesystem::exists(final_path, status_error)) {
        if (status_error) {
            *error = "cannot inspect logical order final generation";
        } else {
            *error = "logical order final generation already exists";
        }
        return false;
    }
    if (status_error) {
        *error = "cannot inspect logical order final generation";
        return false;
    }
    status_error.clear();
    if (std::filesystem::exists(temporary_path, status_error)) {
        if (status_error || !std::filesystem::remove(temporary_path, status_error) ||
            status_error) {
            *error = "cannot clear stale logical order temporary generation";
            return false;
        }
    } else if (status_error) {
        *error = "cannot inspect logical order temporary generation";
        return false;
    }

    std::vector<std::byte> bytes;
    if (!serialize_logical_order_snapshot(
            generation, source_record_indices, &bytes, error)) {
        return false;
    }
    std::ofstream stream(temporary_path, std::ios::binary | std::ios::trunc);
    if (!stream || !write_all(stream, bytes)) {
        *error = "cannot write logical order temporary generation";
        return false;
    }
    stream.flush();
    if (!stream) {
        *error = "cannot flush logical order temporary generation";
        return false;
    }
    stream.close();
    if (!durable_flush_logical_order_file(temporary_path, error)) {
        return false;
    }
    if (!publish_logical_order_temp_file(temporary_path, final_path, error)) {
        return false;
    }

    LogicalOrderSnapshot committed;
    if (!load_logical_order_snapshot(
            store_root, logical_records, &committed, error)) {
        return false;
    }
    if (!committed.persisted || committed.generation != generation ||
        committed.source_record_indices.size() != source_record_indices.size() ||
        !std::equal(
            committed.source_record_indices.begin(),
            committed.source_record_indices.end(),
            source_record_indices.begin(),
            source_record_indices.end())) {
        *error = "committed logical order generation verification mismatch";
        return false;
    }
    return true;
}
