#include "massivedoc_generation.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
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
    std::mt19937_64 random(0x4d3347454e45524ULL);
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("zevryon-") + std::string(name) + "-" +
                       std::to_string(random()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root / "segments", error);
    require(!error, "cannot create temp root");
    return root;
}

void cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "cleanup failed");
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(stream), "cannot create fixture file");
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(stream), "cannot write fixture file");
}

struct Fixture {
    std::filesystem::path root;
    std::vector<std::byte> authority;
    std::array<std::uint8_t, 32> identity{};
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
    for (std::size_t index = 0U; index < fixture.authority.size(); ++index) {
        fixture.authority[index] =
            static_cast<std::byte>(static_cast<unsigned char>(index & 0xffU));
    }
    for (std::size_t index = 0U; index < fixture.identity.size(); ++index) {
        fixture.identity[index] = static_cast<std::uint8_t>(index + 1U);
    }
    fixture.segments.push_back({0U, 7U});
    return fixture;
}

void test_complete_publication_recovers_exact_authority() {
    auto fixture = make_fixture("generation-complete");
    std::string error;
    require(
        zevryon::massivedoc::publish_store_generation(
            fixture.root,
            1U,
            fixture.authority,
            fixture.identity,
            fixture.segments,
            zevryon::massivedoc::GenerationPublicationCut::none,
            &error),
        error);

    zevryon::massivedoc::GenerationRecovery recovery;
    require(
        zevryon::massivedoc::recover_store_generation(
            fixture.root, &recovery, &error),
        error);
    require(recovery.protocol_present, "generation protocol was not detected");
    require(recovery.found, "committed generation was not recovered");
    require(recovery.generation == 1U, "wrong committed generation");
    require(recovery.source_identity == fixture.identity, "source identity mismatch");
    require(recovery.authority_manifest == fixture.authority, "authority manifest mismatch");
    require(recovery.segments.size() == 1U, "segment inventory count mismatch");
    require(recovery.segments[0].segment_id == 0U, "segment id mismatch");
    require(recovery.segments[0].byte_length == 7U, "segment length mismatch");
    cleanup(fixture.root);
}

void test_prepare_only_never_becomes_authority() {
    auto fixture = make_fixture("generation-prepare");
    std::string error;
    require(
        zevryon::massivedoc::publish_store_generation(
            fixture.root,
            1U,
            fixture.authority,
            fixture.identity,
            fixture.segments,
            zevryon::massivedoc::GenerationPublicationCut::after_prepare,
            &error),
        error);
    zevryon::massivedoc::GenerationRecovery recovery;
    require(
        zevryon::massivedoc::recover_store_generation(
            fixture.root, &recovery, &error),
        error);
    require(recovery.protocol_present, "prepare did not establish protocol");
    require(!recovery.found, "prepare-only generation became authority");
    cleanup(fixture.root);
}

void test_published_manifest_without_commit_never_becomes_authority() {
    auto fixture = make_fixture("generation-manifest");
    std::string error;
    require(
        zevryon::massivedoc::publish_store_generation(
            fixture.root,
            1U,
            fixture.authority,
            fixture.identity,
            fixture.segments,
            zevryon::massivedoc::GenerationPublicationCut::after_manifest,
            &error),
        error);
    require(
        std::filesystem::exists(
            zevryon::massivedoc::store_generation_path(fixture.root, 1U)),
        "published generation manifest is missing");

    zevryon::massivedoc::GenerationRecovery recovery;
    require(
        zevryon::massivedoc::recover_store_generation(
            fixture.root, &recovery, &error),
        error);
    require(!recovery.found, "uncommitted published manifest became authority");
    cleanup(fixture.root);
}

void test_corrupt_journal_tail_is_quarantined_and_repaired() {
    auto fixture = make_fixture("generation-journal-tail");
    std::string error;
    require(
        zevryon::massivedoc::publish_store_generation(
            fixture.root,
            1U,
            fixture.authority,
            fixture.identity,
            fixture.segments,
            zevryon::massivedoc::GenerationPublicationCut::none,
            &error),
        error);
    const auto journal =
        zevryon::massivedoc::store_generation_journal_path(fixture.root);
    {
        std::ofstream stream(journal, std::ios::binary | std::ios::app);
        const std::array<char, 7> junk{'b', 'a', 'd', 't', 'a', 'i', 'l'};
        stream.write(junk.data(), static_cast<std::streamsize>(junk.size()));
        require(static_cast<bool>(stream), "cannot append corrupt journal tail");
    }
    const auto corrupted_size = std::filesystem::file_size(journal);

    zevryon::massivedoc::GenerationRecovery recovery;
    require(
        zevryon::massivedoc::recover_store_generation(
            fixture.root, &recovery, &error),
        error);
    require(recovery.found, "valid generation lost after journal tail corruption");
    require(recovery.journal_tail_quarantined, "journal tail was not quarantined");
    require(
        std::filesystem::file_size(journal) + 7U == corrupted_size,
        "generation journal was not repaired to valid prefix");

    const auto quarantine = fixture.root / "quarantine";
    std::size_t quarantined_files = 0U;
    for (const auto& entry : std::filesystem::directory_iterator(quarantine)) {
        if (entry.is_regular_file()) {
            ++quarantined_files;
        }
    }
    require(quarantined_files >= 1U, "journal quarantine artifact missing");
    cleanup(fixture.root);
}

void test_corrupt_newest_manifest_falls_back_to_older_commit() {
    auto fixture = make_fixture("generation-fallback");
    std::string error;
    require(
        zevryon::massivedoc::publish_store_generation(
            fixture.root,
            1U,
            fixture.authority,
            fixture.identity,
            fixture.segments,
            zevryon::massivedoc::GenerationPublicationCut::none,
            &error),
        error);

    auto authority2 = fixture.authority;
    authority2[0] = std::byte{0x5a};
    auto identity2 = fixture.identity;
    identity2[0] = 0xaaU;
    require(
        zevryon::massivedoc::publish_store_generation(
            fixture.root,
            2U,
            authority2,
            identity2,
            fixture.segments,
            zevryon::massivedoc::GenerationPublicationCut::none,
            &error),
        error);

    const auto newest =
        zevryon::massivedoc::store_generation_path(fixture.root, 2U);
    {
        std::fstream stream(
            newest, std::ios::binary | std::ios::in | std::ios::out);
        require(static_cast<bool>(stream), "cannot open generation for corruption");
        char value = '\0';
        stream.read(&value, 1);
        require(static_cast<bool>(stream), "cannot read generation byte");
        value = static_cast<char>(value ^ 0x7f);
        stream.seekp(0);
        stream.write(&value, 1);
        require(static_cast<bool>(stream), "cannot corrupt generation byte");
    }

    zevryon::massivedoc::GenerationRecovery recovery;
    require(
        zevryon::massivedoc::recover_store_generation(
            fixture.root, &recovery, &error),
        error);
    require(recovery.found, "no fallback generation recovered");
    require(recovery.generation == 1U, "did not fall back to older committed generation");
    require(recovery.authority_manifest == fixture.authority, "fallback authority mismatch");
    require(recovery.quarantined_manifests == 1U, "corrupt generation was not quarantined");
    require(!std::filesystem::exists(newest), "corrupt generation remained in authority directory");
    cleanup(fixture.root);
}

} // namespace

int main() {
    test_complete_publication_recovers_exact_authority();
    test_prepare_only_never_becomes_authority();
    test_published_manifest_without_commit_never_becomes_authority();
    test_corrupt_journal_tail_is_quarantined_and_repaired();
    test_corrupt_newest_manifest_falls_back_to_older_commit();
    std::cout << "Zevryon MassiveDoc generation tests passed\n";
    return 0;
}
