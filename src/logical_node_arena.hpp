#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace zevryon::massivedoc {

inline constexpr std::uint64_t kNoLogicalNodeOrdinal = ~std::uint64_t{0};

enum class LogicalSemanticKind : std::uint8_t {
    tag = 0,
    role = 1,
    style = 2,
    attribute_name = 3,
    attribute_value = 4,
};

struct LogicalNodeArenaBuildConfig {
    std::string candidate_commit;
    std::string candidate_tree;
    std::array<std::uint8_t, 32> source_sha256{};
    std::uint32_t semantic_bucket_count{4096U};
    std::uint32_t semantic_hash_bits{64U};
};

struct LogicalNodeAttributeInput {
    std::string_view name;
    std::string_view value;
    std::uint32_t flags{0U};
};

struct LogicalNodeInput {
    std::uint64_t logical_id{0U};
    std::uint64_t source_record_index{0U};
    std::uint64_t source_byte_offset{0U};
    std::uint64_t source_byte_length{0U};
    std::uint64_t parent_ordinal{kNoLogicalNodeOrdinal};
    std::string_view tag;
    std::string_view role;
    std::string_view style;
    std::uint32_t flags{0U};
};

struct LogicalNodeRecord {
    std::uint64_t logical_id{0U};
    std::uint64_t source_record_index{0U};
    std::uint64_t source_byte_offset{0U};
    std::uint64_t source_byte_length{0U};
    std::uint64_t parent_ordinal{kNoLogicalNodeOrdinal};
    std::uint64_t first_child_ordinal{kNoLogicalNodeOrdinal};
    std::uint64_t next_sibling_ordinal{kNoLogicalNodeOrdinal};
    std::uint64_t attribute_offset{0U};
    std::uint32_t attribute_count{0U};
    std::uint32_t tag_id{0U};
    std::uint32_t role_id{0U};
    std::uint32_t style_id{0U};
    std::uint32_t flags{0U};
};

struct LogicalNodeAttributeRecord {
    std::uint32_t name_id{0U};
    std::uint32_t value_id{0U};
    std::uint32_t flags{0U};
};

struct LogicalNodeArenaManifest {
    std::uint32_t format_version{0U};
    std::uint32_t semantic_bucket_count{0U};
    std::uint32_t semantic_hash_bits{0U};
    std::uint64_t node_count{0U};
    std::uint64_t attribute_count{0U};
    std::array<std::uint32_t, 5> semantic_counts{};
    std::string candidate_commit;
    std::string candidate_tree;
    std::array<std::uint8_t, 32> source_sha256{};
};

class LogicalNodeArenaWriter {
public:
    LogicalNodeArenaWriter(
        std::filesystem::path store_root,
        LogicalNodeArenaBuildConfig config);
    ~LogicalNodeArenaWriter();

    LogicalNodeArenaWriter(const LogicalNodeArenaWriter&) = delete;
    LogicalNodeArenaWriter& operator=(const LogicalNodeArenaWriter&) = delete;
    LogicalNodeArenaWriter(LogicalNodeArenaWriter&&) noexcept;
    LogicalNodeArenaWriter& operator=(LogicalNodeArenaWriter&&) noexcept;

    bool begin(std::string* error);
    bool append_node(
        const LogicalNodeInput& node,
        std::span<const LogicalNodeAttributeInput> attributes,
        std::string* error);
    bool finish(std::string* error);

    std::uint64_t node_count() const noexcept;
    std::uint64_t attribute_count() const noexcept;
    std::uint64_t semantic_bucket_head_bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class LogicalNodeArenaReader {
public:
    explicit LogicalNodeArenaReader(std::filesystem::path store_root);
    ~LogicalNodeArenaReader();

    LogicalNodeArenaReader(const LogicalNodeArenaReader&) = delete;
    LogicalNodeArenaReader& operator=(const LogicalNodeArenaReader&) = delete;
    LogicalNodeArenaReader(LogicalNodeArenaReader&&) noexcept;
    LogicalNodeArenaReader& operator=(LogicalNodeArenaReader&&) noexcept;

    bool open(std::string* error);
    const LogicalNodeArenaManifest& manifest() const noexcept;

    bool node_by_ordinal(
        std::uint64_t ordinal,
        LogicalNodeRecord* node,
        std::string* error) const;
    bool node_by_id(
        std::uint64_t logical_id,
        LogicalNodeRecord* node,
        std::string* error) const;
    bool attribute_by_ordinal(
        std::uint64_t ordinal,
        LogicalNodeAttributeRecord* attribute,
        std::string* error) const;
    bool resolve_semantic(
        LogicalSemanticKind kind,
        std::uint32_t id,
        std::string* value,
        std::string* error) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zevryon::massivedoc
