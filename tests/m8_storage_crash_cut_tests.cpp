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

using zevryon::massivedoc::GenerationCompactionConfig;
using zevryon::massivedoc::GenerationCompactionCut;
using zevryon::massivedoc::GenerationCompactionResult;
using zevryon::massivedoc::GenerationPublicationCut;
using zevryon::massivedoc::GenerationRecovery;
using zevryon::massivedoc::GenerationSegmentInventory;

static_assert(static_cast<std::uint32_t>(GenerationPublicationCut::none) == 0U);
static_assert(static_cast<std::uint32_t>(GenerationPublicationCut::after_prepare) == 1U);
static_assert(static_cast<std::uint32_t>(GenerationPublicationCut::after_manifest) == 2U);
static_assert(static_cast<std::uint32_t>(GenerationCompactionCut::none) == 0U);
static_assert(static_cast<std::uint32_t>(GenerationCompactionCut::after_journal_temp) == 1U);
static_assert(static_cast<std::uint32_t>(GenerationCompactionCut::after_journal_replace) == 2U);

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
    std::mt19937_64 random(0x4d3853544f524147ULL);
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("zevryon-") + std::string(name) + "-" +
                       std::to_string(random()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "cannot clear M8 crash-cut fixture root");
    error.clear();
    std::filesystem::create_directories(root / "segments", error);
    require(!error, "cannot create M8 crash-cut fixture segments");
    return root;
}

void cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "M8 crash-cut fixture cleanup failed");
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(stream), "cannot create M8 crash-cut fixture file");
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(stream), "cannot write M8 crash-cut fixture file");
}

struct Fixture {
    std::filesystem::path root;
    std::vector<std::byte> authority;
    std::vector<GenerationSegmentInventory> segments;
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
            (generation * 17U + static_cast<std::uint64_t>(index)) & 0xffU);
    }
    return identity;
}

void publish(
    Fixture* fixture,
    std::uint64_t generation,
    GenerationPublicationCut cut) {
    require(fixture != nullptr, "invalid M8 crash-cut fixture");
    fixture->authority[0] = static_cast<std::byte>(
        static_cast<unsigned char>(generation & 0xffU));
    const auto identity = identity_for(generation);
    std::string error;
    require(
        zevryon::massivedoc::publish_store_generation(
            fixture->root,
            generation,
            fixture->authority,
            identity,
            fixture->segments,
            cut,
            &error),
        error);
}

GenerationRecovery recover(const std::filesystem::path& root) {
    GenerationRecovery recovery;
    std::string error;
    require(
        zevryon::massivedoc::recover_store_generation(root, &recovery, &error),
        error);
    return recovery;
}

void require_generation(
    const GenerationRecovery& recovery,
    std::uint64_t generation,
    const std::string& context) {
    require(recovery.found, context + ": committed authority was lost");
    require(recovery.generation == generation, context + ": wrong authority generation");
    require(
        recovery.source_identity == identity_for(generation),
        context + ": authority source identity drifted");
}

std::size_t count_quarantine_files_with_suffix(
    const std::filesystem::path& root,
    std::string_view suffix) {
    const auto quarantine = root / "quarantine";
    std::error_code exists_error;
    if (!std::filesystem::exists(quarantine, exists_error)) {
        require(!exists_error, "cannot inspect M8 quarantine directory");
        return 0U;
    }
    std::size_t count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator(quarantine)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().find(suffix) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

void test_payload_flush_cut_cannot_create_authority() {
    auto fixture = make_fixture("m8-payload-flush-cut");
    publish(&fixture, 1U, GenerationPublicationCut::after_payload_flush);
    const auto recovery = recover(fixture.root);
    require(!recovery.protocol_present, "payload-flush cut created journal protocol state");
    require(!recovery.found, "payload-flush cut created committed authority");
    require(
        !std::filesystem::exists(
            zevryon::massivedoc::store_generation_path(fixture.root, 1U)),
        "payload-flush cut published a generation manifest");

    publish(&fixture, 1U, GenerationPublicationCut::none);
    require_generation(recover(fixture.root), 1U, "payload-flush restart");
    cleanup(fixture.root);
}

void test_all_precommit_cuts_preserve_previous_authority() {
    const std::array<GenerationPublicationCut, 3> cuts{
        GenerationPublicationCut::after_prepare,
        GenerationPublicationCut::after_manifest_temp,
        GenerationPublicationCut::after_manifest,
    };
    const std::array<std::string_view, 3> names{
        "after-prepare",
        "after-manifest-temp",
        "after-manifest",
    };

    for (std::size_t index = 0U; index < cuts.size(); ++index) {
        auto fixture = make_fixture(std::string("m8-") + std::string(names[index]));
        publish(&fixture, 1U, GenerationPublicationCut::none);
        publish(&fixture, 2U, cuts[index]);
        require_generation(
            recover(fixture.root),
            1U,
            std::string(names[index]) + " crash recovery");

        publish(&fixture, 2U, GenerationPublicationCut::none);
        require_generation(
            recover(fixture.root),
            2U,
            std::string(names[index]) + " restart completion");
        if (cuts[index] == GenerationPublicationCut::after_manifest) {
            require(
                count_quarantine_files_with_suffix(fixture.root, ".uncommitted") == 1U,
                "published-uncommitted manifest was not preserved in quarantine before retry");
        }
        cleanup(fixture.root);
    }
}

void test_commit_cut_is_recoverable_without_postwrite_verifier() {
    auto fixture = make_fixture("m8-after-commit");
    publish(&fixture, 1U, GenerationPublicationCut::none);
    publish(&fixture, 2U, GenerationPublicationCut::after_commit);
    require_generation(recover(fixture.root), 2U, "after-commit crash recovery");
    cleanup(fixture.root);
}

void test_compaction_post_quarantine_cut_is_recoverable_and_resumable() {
    auto fixture = make_fixture("m8-compaction-quarantine");
    for (std::uint64_t generation = 1U; generation <= 4U; ++generation) {
        publish(&fixture, generation, GenerationPublicationCut::none);
    }

    GenerationCompactionResult result;
    std::string error;
    require(
        zevryon::massivedoc::compact_store_generation_metadata(
            fixture.root,
            GenerationCompactionConfig{2U},
            GenerationCompactionCut::after_stale_quarantine,
            &result,
            &error),
        error);
    require(
        result.quarantined_stale_manifests == 1U,
        "post-quarantine cut did not stop after the first durable stale quarantine");
    require_generation(recover(fixture.root), 4U, "post-quarantine crash recovery");
    require(
        count_quarantine_files_with_suffix(fixture.root, ".stale") == 1U,
        "post-quarantine crash receipt count drifted");

    require(
        zevryon::massivedoc::compact_store_generation_metadata(
            fixture.root,
            GenerationCompactionConfig{2U},
            GenerationCompactionCut::none,
            &result,
            &error),
        error);
    require_generation(recover(fixture.root), 4U, "post-quarantine resumed compaction");
    require(
        count_quarantine_files_with_suffix(fixture.root, ".stale") == 2U,
        "resumed compaction did not quarantine every stale manifest");
    cleanup(fixture.root);
}

} // namespace

int main() {
    test_payload_flush_cut_cannot_create_authority();
    test_all_precommit_cuts_preserve_previous_authority();
    test_commit_cut_is_recoverable_without_postwrite_verifier();
    test_compaction_post_quarantine_cut_is_recoverable_and_resumable();
    std::cout << "Zevryon M8 storage crash-cut authority tests passed\n";
    return 0;
}
