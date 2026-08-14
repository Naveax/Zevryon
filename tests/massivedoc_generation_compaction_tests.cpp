#include "massivedoc_generation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

std::filesystem::path temp_root(std::string_view name) {
    std::mt19937_64 random(0x4d33434f4d504143ULL);
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("zevryon-") + std::string(name) + "-" +
                       std::to_string(random()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "cannot clear compaction fixture root");
    std::filesystem::create_directories(root / "segments", error);
    require(!error, "cannot create compaction fixture segments");
    return root;
}

void cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "compaction fixture cleanup failed");
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(stream), "cannot create compaction fixture file");
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(stream), "cannot write compaction fixture file");
}

struct Fixture {
    std::filesystem::path root;
    std::vector<std::byte> authority;
    std::vector<zevryon::massivedoc::GenerationSegmentInventory> segments;
};

Fixture make_fixture(std::string_view name) {
    Fixture fixture;
    fixture.root = temp_root(name);
    write_text(fixture.root / "records.idx", "records");
    write_text(fixture.root / "chunks.idx", "chunks");
    write_text(fixture.root / "search.bgm", "search");
    write_text(fixture.root / "segments" / "segment-00000000.bin", "payload");
    fixture.authority.assign(160U, std::byte{0});
    fixture.segments.push_back({0U, 7U});
    return fixture;
}

std::array<std::uint8_t, 32> identity_for(std::uint64_t generation) {
    std::array<std::uint8_t, 32> identity{};
    for (std::size_t index = 0U; index < identity.size(); ++index) {
        identity[index] = static_cast<std::uint8_t>(
            (generation + static_cast<std::uint64_t>(index)) & 0xffU);
    }
    return identity;
}

void publish_generations(Fixture* fixture, std::uint64_t count) {
    require(fixture != nullptr, "invalid compaction fixture");
    std::string error;
    for (std::uint64_t generation = 1U; generation <= count; ++generation) {
        fixture->authority[0] = static_cast<std::byte>(
            static_cast<unsigned char>(generation & 0xffU));
        const auto identity = identity_for(generation);
        require(
            zevryon::massivedoc::publish_store_generation(
                fixture->root,
                generation,
                fixture->authority,
                identity,
                fixture->segments,
                zevryon::massivedoc::GenerationPublicationCut::none,
                &error),
            error);
    }
}

zevryon::massivedoc::GenerationRecovery recover(
    const std::filesystem::path& root) {
    zevryon::massivedoc::GenerationRecovery recovery;
    std::string error;
    require(
        zevryon::massivedoc::recover_store_generation(root, &recovery, &error),
        error);
    return recovery;
}

std::size_t count_stale_quarantine_files(
    const std::filesystem::path& root) {
    const auto quarantine = root / "quarantine";
    std::error_code exists_error;
    if (!std::filesystem::exists(quarantine, exists_error)) {
        require(!exists_error, "cannot inspect compaction quarantine directory");
        return 0U;
    }
    std::size_t count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator(quarantine)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().find(".stale") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

void corrupt_first_byte(const std::filesystem::path& path) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    require(static_cast<bool>(stream), "cannot open generation for compaction fallback corruption");
    char value = '\0';
    stream.read(&value, 1);
    require(static_cast<bool>(stream), "cannot read generation corruption byte");
    value = static_cast<char>(value ^ 0x7f);
    stream.seekp(0);
    stream.write(&value, 1);
    require(static_cast<bool>(stream), "cannot write generation corruption byte");
}

void test_compaction_retains_authority_and_one_fallback() {
    auto fixture = make_fixture("generation-compaction");
    publish_generations(&fixture, 4U);
    const auto before = recover(fixture.root);
    require(before.found && before.generation == 4U, "missing pre-compaction authority");

    zevryon::massivedoc::GenerationCompactionResult result;
    std::string error;
    require(
        zevryon::massivedoc::compact_store_generation_metadata(
            fixture.root,
            zevryon::massivedoc::GenerationCompactionConfig{2U},
            zevryon::massivedoc::GenerationCompactionCut::none,
            &result,
            &error),
        error);
    require(result.authority_generation == 4U, "compaction authority generation mismatch");
    require(result.retained_committed_generations == 2U, "compaction retention count mismatch");
    require(result.quarantined_stale_manifests == 2U, "compaction stale quarantine count mismatch");
    require(result.journal_bytes_after < result.journal_bytes_before, "compaction did not shrink journal");
    require(
        std::filesystem::file_size(
            zevryon::massivedoc::store_generation_journal_path(fixture.root)) ==
            result.journal_bytes_after,
        "compacted journal size mismatch");
    require(
        !std::filesystem::exists(
            zevryon::massivedoc::store_generation_path(fixture.root, 1U)) &&
        !std::filesystem::exists(
            zevryon::massivedoc::store_generation_path(fixture.root, 2U)),
        "stale generation manifests survived completed compaction");
    require(
        std::filesystem::exists(
            zevryon::massivedoc::store_generation_path(fixture.root, 3U)) &&
        std::filesystem::exists(
            zevryon::massivedoc::store_generation_path(fixture.root, 4U)),
        "retained generation manifests were removed");
    require(count_stale_quarantine_files(fixture.root) == 2U, "stale quarantine artifacts mismatch");

    const auto after = recover(fixture.root);
    require(after.found && after.generation == 4U, "compaction changed current authority");
    require(after.source_identity == identity_for(4U), "compaction changed authority identity");

    corrupt_first_byte(
        zevryon::massivedoc::store_generation_path(fixture.root, 4U));
    const auto fallback = recover(fixture.root);
    require(fallback.found && fallback.generation == 3U, "compaction removed required recovery fallback");
    require(fallback.source_identity == identity_for(3U), "fallback identity mismatch after compaction");
    cleanup(fixture.root);
}

void test_crash_after_compaction_temp_preserves_old_journal() {
    auto fixture = make_fixture("generation-compaction-temp");
    publish_generations(&fixture, 4U);
    const auto journal = zevryon::massivedoc::store_generation_journal_path(fixture.root);
    const std::uint64_t before_bytes = std::filesystem::file_size(journal);

    zevryon::massivedoc::GenerationCompactionResult result;
    std::string error;
    require(
        zevryon::massivedoc::compact_store_generation_metadata(
            fixture.root,
            zevryon::massivedoc::GenerationCompactionConfig{2U},
            zevryon::massivedoc::GenerationCompactionCut::after_journal_temp,
            &result,
            &error),
        error);
    require(std::filesystem::file_size(journal) == before_bytes, "temp-cut compaction replaced live journal");
    for (std::uint64_t generation = 1U; generation <= 4U; ++generation) {
        require(
            std::filesystem::exists(
                zevryon::massivedoc::store_generation_path(fixture.root, generation)),
            "temp-cut compaction removed generation manifest");
    }
    const auto recovered = recover(fixture.root);
    require(recovered.found && recovered.generation == 4U, "temp-cut compaction changed authority");

    require(
        zevryon::massivedoc::compact_store_generation_metadata(
            fixture.root,
            zevryon::massivedoc::GenerationCompactionConfig{2U},
            zevryon::massivedoc::GenerationCompactionCut::none,
            &result,
            &error),
        error);
    require(result.quarantined_stale_manifests == 2U, "resumed temp-cut compaction did not quarantine stale manifests");
    cleanup(fixture.root);
}

void test_crash_after_journal_replace_is_resumable() {
    auto fixture = make_fixture("generation-compaction-replace");
    publish_generations(&fixture, 4U);

    zevryon::massivedoc::GenerationCompactionResult result;
    std::string error;
    require(
        zevryon::massivedoc::compact_store_generation_metadata(
            fixture.root,
            zevryon::massivedoc::GenerationCompactionConfig{2U},
            zevryon::massivedoc::GenerationCompactionCut::after_journal_replace,
            &result,
            &error),
        error);
    require(result.journal_bytes_after < result.journal_bytes_before, "replace-cut journal was not compacted");
    require(
        std::filesystem::exists(
            zevryon::massivedoc::store_generation_path(fixture.root, 1U)) &&
        std::filesystem::exists(
            zevryon::massivedoc::store_generation_path(fixture.root, 2U)),
        "replace-cut compaction quarantined stale manifests before publication boundary");
    const auto recovered = recover(fixture.root);
    require(recovered.found && recovered.generation == 4U, "replace-cut compaction changed authority");

    require(
        zevryon::massivedoc::compact_store_generation_metadata(
            fixture.root,
            zevryon::massivedoc::GenerationCompactionConfig{2U},
            zevryon::massivedoc::GenerationCompactionCut::none,
            &result,
            &error),
        error);
    require(result.quarantined_stale_manifests == 2U, "resumed replace-cut compaction missed stale manifests");
    require(count_stale_quarantine_files(fixture.root) == 2U, "replace-cut stale quarantine artifact mismatch");
    cleanup(fixture.root);
}

void test_compaction_rejects_unbounded_retention() {
    auto fixture = make_fixture("generation-compaction-bounds");
    publish_generations(&fixture, 1U);
    zevryon::massivedoc::GenerationCompactionResult result;
    std::string error;
    require(
        !zevryon::massivedoc::compact_store_generation_metadata(
            fixture.root,
            zevryon::massivedoc::GenerationCompactionConfig{0U},
            zevryon::massivedoc::GenerationCompactionCut::none,
            &result,
            &error),
        "zero compaction retention was accepted");
    require(
        error == "generation compaction retention is outside the supported bounded range",
        "zero compaction retention diagnostic mismatch");
    require(
        !zevryon::massivedoc::compact_store_generation_metadata(
            fixture.root,
            zevryon::massivedoc::GenerationCompactionConfig{65U},
            zevryon::massivedoc::GenerationCompactionCut::none,
            &result,
            &error),
        "oversized compaction retention was accepted");
    cleanup(fixture.root);
}

} // namespace

int main() {
    test_compaction_retains_authority_and_one_fallback();
    test_crash_after_compaction_temp_preserves_old_journal();
    test_crash_after_journal_replace_is_resumable();
    test_compaction_rejects_unbounded_retention();
    std::cout << "Zevryon MassiveDoc generation compaction tests passed\n";
    return 0;
}
