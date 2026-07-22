std::string hex_digest(const std::array<std::uint8_t, 32>& digest) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const std::uint8_t byte : digest) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>((value >> (index * 8U)) & static_cast<T>(0xffU)));
    }
}

template <typename T>
std::optional<T> read_le(std::span<const std::byte> bytes, std::size_t offset) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
        return std::nullopt;
    }
    T value = 0;
