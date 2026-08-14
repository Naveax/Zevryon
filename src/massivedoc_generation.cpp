#include "massivedoc_generation.hpp"
#include "massivedoc_generation_sync.hpp"

namespace zevryon::massivedoc {

bool recover_store_generation_unlocked(
    const std::filesystem::path& store_root,
    GenerationRecovery* recovery,
    std::string* error);

} // namespace zevryon::massivedoc

#define publish_store_generation publish_store_generation_unlocked
#define publish_legacy_store_manifest publish_legacy_store_manifest_unlocked
#define recover_store_generation recover_store_generation_unlocked
#include "massivedoc_generation_part00.inc"
#undef recover_store_generation
#undef publish_legacy_store_manifest
#undef publish_store_generation

#define compact_store_generation_metadata compact_store_generation_metadata_unlocked
#define recover_store_generation recover_store_generation_unlocked
#include "massivedoc_generation_part06.inc"
#undef recover_store_generation
#undef compact_store_generation_metadata

namespace zevryon::massivedoc {

bool publish_store_generation(
    const std::filesystem::path& store_root,
    std::uint64_t generation,
    std::span<const std::byte> authority_manifest,
    const std::array<std::uint8_t, 32>& source_identity,
    std::span<const GenerationSegmentInventory> segments,
    GenerationPublicationCut cut,
    std::string* error) {
    std::lock_guard<std::recursive_mutex> transaction_lock(
        generation_transaction_mutex());
    return publish_store_generation_unlocked(
        store_root,
        generation,
        authority_manifest,
        source_identity,
        segments,
        cut,
        error);
}

bool publish_legacy_store_manifest(
    const std::filesystem::path& store_root,
    std::span<const std::byte> authority_manifest,
    std::string* error) {
    std::lock_guard<std::recursive_mutex> transaction_lock(
        generation_transaction_mutex());
    return publish_legacy_store_manifest_unlocked(
        store_root,
        authority_manifest,
        error);
}

bool recover_store_generation(
    const std::filesystem::path& store_root,
    GenerationRecovery* recovery,
    std::string* error) {
    std::lock_guard<std::recursive_mutex> transaction_lock(
        generation_transaction_mutex());
    return recover_store_generation_unlocked(store_root, recovery, error);
}

bool compact_store_generation_metadata(
    const std::filesystem::path& store_root,
    GenerationCompactionConfig config,
    GenerationCompactionCut cut,
    GenerationCompactionResult* result,
    std::string* error) {
    std::lock_guard<std::recursive_mutex> transaction_lock(
        generation_transaction_mutex());
    return compact_store_generation_metadata_unlocked(
        store_root,
        config,
        cut,
        result,
        error);
}

} // namespace zevryon::massivedoc
