#pragma once

#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

namespace zevryon::text {

using OpenTypeTag = std::uint32_t;

constexpr OpenTypeTag open_type_tag(
    char first,
    char second,
    char third,
    char fourth) noexcept {
    return (static_cast<OpenTypeTag>(static_cast<unsigned char>(first)) << 24U) |
           (static_cast<OpenTypeTag>(static_cast<unsigned char>(second)) << 16U) |
           (static_cast<OpenTypeTag>(static_cast<unsigned char>(third)) << 8U) |
           static_cast<OpenTypeTag>(static_cast<unsigned char>(fourth));
}

inline constexpr OpenTypeTag kOpenTypeTrueTypeVersion = 0x00010000U;
inline constexpr OpenTypeTag kOpenTypeCffVersion = open_type_tag('O', 'T', 'T', 'O');
inline constexpr OpenTypeTag kOpenTypeCollectionTag = open_type_tag('t', 't', 'c', 'f');
inline constexpr OpenTypeTag kOpenTypeHeadTag = open_type_tag('h', 'e', 'a', 'd');
inline constexpr OpenTypeTag kOpenTypeCmapTag = open_type_tag('c', 'm', 'a', 'p');
inline constexpr OpenTypeTag kOpenTypeMaxpTag = open_type_tag('m', 'a', 'x', 'p');
inline constexpr OpenTypeTag kOpenTypeNameTag = open_type_tag('n', 'a', 'm', 'e');
inline constexpr OpenTypeTag kOpenTypeOs2Tag = open_type_tag('O', 'S', '/', '2');
inline constexpr OpenTypeTag kOpenTypeFvarTag = open_type_tag('f', 'v', 'a', 'r');
inline constexpr OpenTypeTag kOpenTypeColrTag = open_type_tag('C', 'O', 'L', 'R');
inline constexpr OpenTypeTag kOpenTypeCpalTag = open_type_tag('C', 'P', 'A', 'L');

struct OpenTypeTableRecord {
    OpenTypeTag tag{0};
    std::uint32_t checksum{0};
    std::uint32_t offset{0};
    std::uint32_t length{0};

    bool operator==(const OpenTypeTableRecord&) const noexcept = default;
};

static_assert(
    sizeof(OpenTypeTableRecord) == 16U,
    "OpenType table records must match the SFNT directory format");

enum class OpenTypeContainerKind : std::uint8_t {
    SingleFont = 0,
    Collection
};

enum class OpenTypeOutlineKind : std::uint8_t {
    TrueType = 0,
    Cff
};

enum class OpenTypeDirectoryErrorKind : std::uint8_t {
    None = 0,
    InvalidInput,
    UnsupportedContainer,
    UnsupportedVersion,
    InvalidCollection,
    FaceIndexOutOfRange,
    DirectoryOutOfBounds,
    InvalidTableCount,
    InvalidSearchFields,
    InvalidTableTag,
    DuplicateOrUnsortedTag,
    EmptyTable,
    MisalignedTable,
    TableOutOfBounds,
    TableOverlapsDirectory,
    TableOverlap,
    NonZeroPadding,
    ChecksumMismatch,
    OutputBudgetExceeded
};

struct OpenTypeDirectoryError {
    OpenTypeDirectoryErrorKind kind{OpenTypeDirectoryErrorKind::None};
    std::size_t face_index{0};
    std::size_t table_index{0};
    OpenTypeTag tag{0};
    std::string message;
};

struct OpenTypeDirectoryOptions {
    std::uint32_t maximum_collection_faces{4096U};
    std::uint16_t maximum_tables{4096U};
    bool reject_invalid_search_fields{false};
    bool verify_table_checksums{true};
    bool require_zero_padding{true};
};

struct OpenTypeDirectoryStats {
    std::uint64_t source_bytes{0};
    std::uint64_t collection_faces{0};
    std::uint64_t tables{0};
    std::uint64_t table_bytes{0};
    std::uint64_t padding_bytes{0};
    std::uint64_t checksums_verified{0};
    std::uint64_t search_field_mismatches{0};
    std::uint64_t required_table_views{0};
};

class OpenTypeDirectory final {
public:
    explicit OpenTypeDirectory(std::size_t hard_limit_bytes);

    OpenTypeDirectory(const OpenTypeDirectory&) = delete;
    OpenTypeDirectory& operator=(const OpenTypeDirectory&) = delete;

    OpenTypeContainerKind container_kind() const noexcept;
    OpenTypeOutlineKind outline_kind() const noexcept;
    std::uint32_t face_index() const noexcept;
    std::uint32_t collection_face_count() const noexcept;
    std::uint32_t directory_offset() const noexcept;
    std::uint32_t sfnt_version() const noexcept;
    std::span<const OpenTypeTableRecord> tables() const noexcept;

    core::ResourceSnapshot resource_snapshot() const noexcept;
    bool accounting_clean() const noexcept;
    bool within_hard_limit() const noexcept;

private:
    friend bool parse_opentype_directory(
        std::span<const std::byte>,
        std::uint32_t,
        const OpenTypeDirectoryOptions&,
        std::size_t,
        std::shared_ptr<const OpenTypeDirectory>*,
        OpenTypeDirectoryStats*,
        OpenTypeDirectoryError*) noexcept;

    core::ResourceLedger ledger_;
    core::LedgerMemoryResource resource_;
    std::pmr::vector<OpenTypeTableRecord> tables_;
    OpenTypeContainerKind container_kind_{OpenTypeContainerKind::SingleFont};
    OpenTypeOutlineKind outline_kind_{OpenTypeOutlineKind::TrueType};
    std::uint32_t face_index_{0};
    std::uint32_t collection_face_count_{1};
    std::uint32_t directory_offset_{0};
    std::uint32_t sfnt_version_{kOpenTypeTrueTypeVersion};
};

const char* open_type_directory_error_kind_name(
    OpenTypeDirectoryErrorKind kind) noexcept;

bool parse_opentype_directory(
    std::span<const std::byte> source,
    std::uint32_t face_index,
    const OpenTypeDirectoryOptions& options,
    std::size_t hard_limit_bytes,
    std::shared_ptr<const OpenTypeDirectory>* output,
    OpenTypeDirectoryStats* stats,
    OpenTypeDirectoryError* error) noexcept;

const OpenTypeTableRecord* find_opentype_table(
    const OpenTypeDirectory& directory,
    OpenTypeTag tag) noexcept;

std::span<const std::byte> opentype_table_bytes(
    std::span<const std::byte> source,
    const OpenTypeDirectory& directory,
    OpenTypeTag tag) noexcept;

std::uint32_t calculate_opentype_table_checksum(
    std::span<const std::byte> table,
    bool zero_head_checksum_adjustment) noexcept;

} // namespace zevryon::text
