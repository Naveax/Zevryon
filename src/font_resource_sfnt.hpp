#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace zevryon::text {

constexpr std::uint32_t sfnt_tag(char a, char b, char c, char d) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

enum class SfntContainerKind : std::uint8_t {
    SingleFont = 0,
    Collection
};

enum class SfntFlavor : std::uint8_t {
    TrueType = 0,
    Cff
};

enum class SfntParseErrorKind : std::uint8_t {
    None = 0,
    InvalidArgument,
    TruncatedInput,
    UnsupportedContainerVersion,
    InvalidCollection,
    InvalidFaceIndex,
    UnsupportedSfntVersion,
    InvalidTableCount,
    InvalidTableTag,
    UnsortedTableDirectory,
    DuplicateTableTag,
    InvalidTableRange,
    DirectoryOverlap,
    TableOverlap,
    ArithmeticOverflow
};

struct SfntParseError {
    SfntParseErrorKind kind{SfntParseErrorKind::None};
    std::size_t byte_offset{0};
    std::uint32_t record_index{0};
    const char* message{nullptr};
};

struct SfntTableRecord {
    std::uint32_t tag{0};
    std::uint32_t checksum{0};
    std::uint32_t offset{0};
    std::uint32_t length{0};

    bool operator==(const SfntTableRecord&) const noexcept = default;
};

struct SfntParseStats {
    std::uint32_t face_count{0};
    std::uint32_t selected_face_index{0};
    std::uint32_t selected_face_offset{0};
    std::uint32_t table_count{0};
    std::uint64_t table_payload_bytes{0};
    std::uint32_t zero_length_tables{0};
    std::uint32_t unaligned_tables{0};
    bool search_parameters_match{false};
};

class SfntResourceView final {
public:
    SfntResourceView() = default;

    bool valid() const noexcept;
    SfntContainerKind container_kind() const noexcept;
    SfntFlavor flavor() const noexcept;
    std::uint32_t face_count() const noexcept;
    std::uint32_t face_index() const noexcept;
    std::uint32_t face_offset() const noexcept;
    std::uint16_t table_count() const noexcept;
    std::span<const std::byte> bytes() const noexcept;

    bool table_record(std::uint16_t index, SfntTableRecord* output) const noexcept;
    bool find_table(std::uint32_t tag, SfntTableRecord* output) const noexcept;
    std::span<const std::byte> table_data(const SfntTableRecord& record) const noexcept;

private:
    friend bool open_sfnt_resource(
        std::span<const std::byte>,
        std::uint32_t,
        SfntResourceView*,
        SfntParseStats*,
        SfntParseError*) noexcept;

    std::span<const std::byte> bytes_{};
    std::uint32_t directory_offset_{0};
    std::uint32_t face_count_{0};
    std::uint32_t face_index_{0};
    std::uint16_t table_count_{0};
    SfntContainerKind container_kind_{SfntContainerKind::SingleFont};
    SfntFlavor flavor_{SfntFlavor::TrueType};
};

const char* sfnt_parse_error_kind_name(SfntParseErrorKind kind) noexcept;

// Opens one face from an OpenType sfnt resource without allocating or retaining
// ownership. The caller must keep `bytes` alive and immutable for the lifetime
// of the returned view. Failure always resets `output` and publishes no view.
bool open_sfnt_resource(
    std::span<const std::byte> bytes,
    std::uint32_t face_index,
    SfntResourceView* output,
    SfntParseStats* stats,
    SfntParseError* error) noexcept;

} // namespace zevryon::text
