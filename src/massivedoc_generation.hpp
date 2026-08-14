#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace zevryon::massivedoc {

struct GenerationSegmentInventory {
    std::uint32_t segment_id{0U};
    std::uint64_t byte_length{0U};
};

enum class GenerationPublicationCut : std::uint32_t {
    none = 0U,
    after_prepare = 1U,
    after_manifest = 2U,
};

struct GenerationRecovery {
    bool protocol_present{false};
    bool found{false};
    bool journal_tail_quarantined{false};
    std::uint64_t generation{0U};
    std::uint64_t quarantined_manifests{0U};
    std::array<std::uint8_t, 32> source_identity{};
    std::vector<std::byte> authority_manifest;
    std::vector<GenerationSegmentInventory> segments;
};

std::filesystem::path store_generation_path(
    const std::filesystem::path& store_root,
    std::uint64_t generation);

std::filesystem::path store_generation_journal_path(
    const std::filesystem::path& store_root);

bool publish_store_generation(
    const std::filesystem::path& store_root,
    std::uint64_t generation,
    std::span<const std::byte> authority_manifest,
    const std::array<std::uint8_t, 32>& source_identity,
    std::span<const GenerationSegmentInventory> segments,
    GenerationPublicationCut cut,
    std::string* error);

bool publish_legacy_store_manifest(
    const std::filesystem::path& store_root,
    std::span<const std::byte> authority_manifest,
    std::string* error);

bool recover_store_generation(
    const std::filesystem::path& store_root,
    GenerationRecovery* recovery,
    std::string* error);

} // namespace zevryon::massivedoc
