namespace {

constexpr std::array<char, 8> kLogicalOrderMagic{'Z', 'V', 'O', 'R', 'D', '0', '0', '1'};
constexpr std::uint32_t kLogicalOrderVersion = 1U;
constexpr std::size_t kLogicalOrderHeaderBytes = 40U;
constexpr std::size_t kLogicalOrderTrailerBytes = sizeof(std::uint32_t);
constexpr std::size_t kLogicalOrderEntryBytes = sizeof(std::uint64_t);
constexpr std::string_view kLogicalOrderPrefix{"logical-order.g"};
constexpr std::string_view kLogicalOrderSuffix{".zmd"};
constexpr std::size_t kLogicalOrderGenerationDigits = 16U;

std::uint32_t logical_order_crc32_update(
    std::uint32_t crc,
    std::span<const std::byte> bytes) noexcept {
    crc = ~crc;
    for (const std::byte raw : bytes) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(raw));
        for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

bool identity_logical_order(
    std::uint64_t expected_records,
    LogicalOrderSnapshot* snapshot,
    std::string* error) {
    if (expected_records > static_cast<std::uint64_t>(
                               std::numeric_limits<std::size_t>::max())) {
        *error = "logical order record count exceeds addressable memory";
        return false;
    }
    snapshot->persisted = false;
    snapshot->generation = 0U;
    snapshot->source_record_indices.clear();
    snapshot->source_record_indices.reserve(static_cast<std::size_t>(expected_records));
    for (std::uint64_t index = 0U; index < expected_records; ++index) {
        snapshot->source_record_indices.push_back(index);
    }
    return true;
}

std::string logical_order_generation_filename(
    std::uint64_t generation,
    bool temporary) {
    std::ostringstream output;
    output << kLogicalOrderPrefix << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(static_cast<int>(kLogicalOrderGenerationDigits)) << generation
           << kLogicalOrderSuffix;
    if (temporary) {
        output << ".tmp";
    }
    return output.str();
}

bool parse_generation_filename(
    std::string_view name,
    std::uint64_t* generation) noexcept {
    if (generation == nullptr ||
        name.size() != kLogicalOrderPrefix.size() + kLogicalOrderGenerationDigits +
                           kLogicalOrderSuffix.size() ||
        !name.starts_with(kLogicalOrderPrefix) || !name.ends_with(kLogicalOrderSuffix)) {
        return false;
    }
    const std::string_view digits = name.substr(
        kLogicalOrderPrefix.size(), kLogicalOrderGenerationDigits);
    std::uint64_t value = 0U;
    for (const char character : digits) {
        std::uint64_t nibble = 0U;
        if (character >= '0' && character <= '9') {
            nibble = static_cast<std::uint64_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            nibble = static_cast<std::uint64_t>(character - 'a' + 10);
        } else {
            return false;
        }
        if (value > (std::numeric_limits<std::uint64_t>::max() - nibble) / 16U) {
            return false;
        }
        value = value * 16U + nibble;
    }
    if (value == 0U) {
        return false;
    }
    *generation = value;
    return true;
}

bool read_logical_order_file(
    const std::filesystem::path& path,
    std::uint64_t expected_records,
    LogicalOrderSnapshot* snapshot,
    std::string* error) {
    std::error_code size_error;
    const std::uint64_t file_bytes = std::filesystem::file_size(path, size_error);
    if (size_error || file_bytes > static_cast<std::uint64_t>(
                                      std::numeric_limits<std::size_t>::max())) {
        *error = "cannot size logical order snapshot";
        return false;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(file_bytes));
    std::ifstream stream(path, std::ios::binary);
    if (!stream || !read_exact(stream, bytes)) {
        *error = "cannot read logical order snapshot";
        return false;
    }
    return parse_logical_order_snapshot(bytes, expected_records, snapshot, error);
}

} // namespace

std::filesystem::path logical_order_generation_path(
    const std::filesystem::path& store_root,
    std::uint64_t generation) {
    return store_root / "arena" /
           logical_order_generation_filename(generation, false);
}

std::filesystem::path logical_order_generation_temp_path(
    const std::filesystem::path& store_root,
    std::uint64_t generation) {
    return store_root / "arena" /
           logical_order_generation_filename(generation, true);
}

bool parse_logical_order_snapshot(
    std::span<const std::byte> bytes,
    std::uint64_t expected_records,
    LogicalOrderSnapshot* snapshot,
    std::string* error) {
    if (snapshot == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    *snapshot = LogicalOrderSnapshot{};

    if (bytes.size() < kLogicalOrderHeaderBytes + kLogicalOrderTrailerBytes) {
        *error = "truncated logical order snapshot";
        return false;
    }
    for (std::size_t index = 0U; index < kLogicalOrderMagic.size(); ++index) {
        if (std::to_integer<unsigned char>(bytes[index]) !=
            static_cast<unsigned char>(kLogicalOrderMagic[index])) {
            *error = "invalid logical order snapshot magic";
            return false;
        }
    }

    const auto version = read_le<std::uint32_t>(bytes, 8U);
    const auto header_bytes = read_le<std::uint32_t>(bytes, 12U);
    const auto logical_records = read_le<std::uint64_t>(bytes, 16U);
    const auto generation = read_le<std::uint64_t>(bytes, 24U);
    const auto payload_bytes = read_le<std::uint64_t>(bytes, 32U);
    if (!version || !header_bytes || !logical_records || !generation || !payload_bytes ||
        *version != kLogicalOrderVersion ||
        *header_bytes != kLogicalOrderHeaderBytes) {
        *error = "unsupported logical order snapshot header";
        return false;
    }
    if (*logical_records != expected_records) {
        *error = "logical order snapshot record count mismatch";
        return false;
    }
    if (*generation == 0U) {
        *error = "logical order snapshot generation must be non-zero";
        return false;
    }
    if (expected_records > std::numeric_limits<std::uint64_t>::max() /
                               static_cast<std::uint64_t>(kLogicalOrderEntryBytes)) {
        *error = "logical order snapshot payload size overflow";
        return false;
    }
    const std::uint64_t expected_payload_bytes =
        expected_records * static_cast<std::uint64_t>(kLogicalOrderEntryBytes);
    if (*payload_bytes != expected_payload_bytes) {
        *error = "logical order snapshot payload size mismatch";
        return false;
    }
    if (expected_payload_bytes > static_cast<std::uint64_t>(
                                     std::numeric_limits<std::size_t>::max() -
                                     kLogicalOrderHeaderBytes -
                                     kLogicalOrderTrailerBytes)) {
        *error = "logical order snapshot exceeds addressable memory";
        return false;
    }
    const std::size_t expected_size =
        kLogicalOrderHeaderBytes + static_cast<std::size_t>(expected_payload_bytes) +
        kLogicalOrderTrailerBytes;
    if (bytes.size() != expected_size) {
        *error = "logical order snapshot file size mismatch";
        return false;
    }

    const auto expected_crc = read_le<std::uint32_t>(
        bytes, expected_size - kLogicalOrderTrailerBytes);
    const std::uint32_t actual_crc = logical_order_crc32_update(
        0U, bytes.first(expected_size - kLogicalOrderTrailerBytes));
    if (!expected_crc || *expected_crc != actual_crc) {
        *error = "logical order snapshot CRC mismatch";
        return false;
    }
    if (expected_records > static_cast<std::uint64_t>(
                               std::numeric_limits<std::size_t>::max())) {
        *error = "logical order record count exceeds addressable memory";
        return false;
    }

    std::vector<bool> seen(static_cast<std::size_t>(expected_records), false);
    snapshot->source_record_indices.clear();
    snapshot->source_record_indices.reserve(static_cast<std::size_t>(expected_records));
    for (std::uint64_t logical_index = 0U; logical_index < expected_records;
         ++logical_index) {
        const std::size_t offset =
            kLogicalOrderHeaderBytes +
            static_cast<std::size_t>(logical_index) * kLogicalOrderEntryBytes;
        const auto source_record_index = read_le<std::uint64_t>(bytes, offset);
        if (!source_record_index) {
            *error = "truncated logical order snapshot permutation";
            return false;
        }
        if (*source_record_index >= expected_records) {
            *error = "logical order snapshot source record out of range";
            return false;
        }
        const std::size_t source = static_cast<std::size_t>(*source_record_index);
        if (seen[source]) {
            *error = "logical order snapshot contains duplicate source record";
            return false;
        }
        seen[source] = true;
        snapshot->source_record_indices.push_back(*source_record_index);
    }

    snapshot->persisted = true;
    snapshot->generation = *generation;
    return true;
}

bool load_logical_order_snapshot(
    const std::filesystem::path& store_root,
    std::uint64_t expected_records,
    LogicalOrderSnapshot* snapshot,
    std::string* error) {
    if (snapshot == nullptr || error == nullptr) {
        return false;
    }
    error->clear();
    *snapshot = LogicalOrderSnapshot{};

    const auto arena_root = store_root / "arena";
    std::error_code status_error;
    const bool arena_exists = std::filesystem::exists(arena_root, status_error);
    if (status_error) {
        *error = "cannot inspect logical order arena";
        return false;
    }
    if (!arena_exists) {
        return identity_logical_order(expected_records, snapshot, error);
    }

    bool found = false;
    LogicalOrderSnapshot newest;
    std::filesystem::directory_iterator iterator(arena_root, status_error);
    const std::filesystem::directory_iterator end;
    if (status_error) {
        *error = "cannot enumerate logical order generations";
        return false;
    }
    for (; iterator != end; iterator.increment(status_error)) {
        if (status_error) {
            *error = "cannot enumerate logical order generations";
            return false;
        }
        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code type_error;
        if (!entry.is_regular_file(type_error) || type_error) {
            if (type_error) {
                *error = "cannot inspect logical order generation entry";
                return false;
            }
            continue;
        }
        const std::string name = entry.path().filename().string();
        std::uint64_t filename_generation = 0U;
        if (!parse_generation_filename(name, &filename_generation)) {
            continue;
        }

        LogicalOrderSnapshot candidate;
        std::string candidate_error;
        if (!read_logical_order_file(
                entry.path(), expected_records, &candidate, &candidate_error)) {
            *error = "invalid committed logical order generation " + name +
                     ": " + candidate_error;
            return false;
        }
        if (candidate.generation != filename_generation) {
            *error = "logical order generation filename/header mismatch";
            return false;
        }
        if (!found || candidate.generation > newest.generation) {
            newest = std::move(candidate);
            found = true;
        }
    }
    if (status_error) {
        *error = "cannot enumerate logical order generations";
        return false;
    }
    if (!found) {
        return identity_logical_order(expected_records, snapshot, error);
    }
    *snapshot = std::move(newest);
    return true;
}
