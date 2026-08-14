#include "massivedoc_store.hpp"

#include <array>
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
    std::mt19937_64 random(0x4d3347454e52554eULL);
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("zevryon-") + std::string(name) + "-" + std::to_string(random()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "cannot clear runtime test root");
    return root;
}

void cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "runtime test cleanup failed");
}

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(begin, begin + static_cast<std::ptrdiff_t>(text.size()));
}

void create_store(const std::filesystem::path& root) {
    zevryon::massivedoc::StoreConfig config;
    config.segment_bytes = 32U;
    config.records_per_search_block = 2U;
    zevryon::massivedoc::StoreWriter writer(root, config);
    std::string error;
    const auto first = bytes_of("generation runtime payload alpha");
    const auto second = bytes_of("generation runtime payload beta");
    require(writer.append(41U, first, &error), error);
    require(writer.append(42U, second, &error), error);
    zevryon::massivedoc::StoreStats stats;
    require(writer.finalize({}, &stats, &error), error);
    require(stats.corpus.logical_records == 2U, "runtime store record count mismatch");
}

zevryon::massivedoc::GenerationRecovery recover(const std::filesystem::path& root) {
    zevryon::massivedoc::GenerationRecovery recovery;
    std::string error;
    require(
        zevryon::massivedoc::recover_store_generation(root, &recovery, &error),
        error);
    return recovery;
}

void test_committed_generation_is_reader_authority() {
    const auto root = temp_root("generation-authority");
    create_store(root);
    const auto recovery = recover(root);
    require(recovery.protocol_present, "generation journal was not published");
    require(recovery.found && recovery.generation == 1U, "generation one was not committed");
    require(recovery.authority_manifest.size() == 160U, "authority manifest size mismatch");
    require(!recovery.segments.empty(), "generation segment inventory is empty");

    std::error_code remove_error;
    require(
        std::filesystem::remove(root / "manifest.zmd", remove_error) && !remove_error,
        "legacy compatibility manifest was not available for removal");

    zevryon::massivedoc::StoreReader reader(root);
    std::string error;
    require(reader.open(&error), error);
    require(reader.verify(&error), error);
    cleanup(root);
}

void test_uncommitted_generations_never_supersede_committed_authority() {
    const auto root = temp_root("generation-incomplete");
    create_store(root);
    const auto committed = recover(root);
    require(committed.found && committed.generation == 1U, "missing baseline generation");

    std::string error;
    auto identity_two = committed.source_identity;
    identity_two[0] ^= 0x31U;
    require(
        zevryon::massivedoc::publish_store_generation(
            root,
            2U,
            committed.authority_manifest,
            identity_two,
            committed.segments,
            zevryon::massivedoc::GenerationPublicationCut::after_prepare,
            &error),
        error);

    auto identity_three = committed.source_identity;
    identity_three[0] ^= 0x63U;
    require(
        zevryon::massivedoc::publish_store_generation(
            root,
            3U,
            committed.authority_manifest,
            identity_three,
            committed.segments,
            zevryon::massivedoc::GenerationPublicationCut::after_manifest,
            &error),
        error);

    const auto after_crashes = recover(root);
    require(after_crashes.found, "committed generation disappeared after crash cuts");
    require(after_crashes.generation == 1U, "uncommitted generation became reader authority");

    zevryon::massivedoc::StoreReader reader(root);
    require(reader.open(&error), error);
    require(reader.verify(&error), error);
    cleanup(root);
}

void test_reader_repairs_corrupt_journal_tail() {
    const auto root = temp_root("generation-journal-tail");
    create_store(root);
    const auto journal = zevryon::massivedoc::store_generation_journal_path(root);
    const auto clean_size = std::filesystem::file_size(journal);
    {
        std::ofstream stream(journal, std::ios::binary | std::ios::app);
        const std::array<char, 7> junk{'b', 'a', 'd', 't', 'a', 'i', 'l'};
        stream.write(junk.data(), static_cast<std::streamsize>(junk.size()));
        require(static_cast<bool>(stream), "cannot append corrupt journal tail");
    }

    zevryon::massivedoc::StoreReader reader(root);
    std::string error;
    require(reader.open(&error), error);
    require(reader.verify(&error), error);
    require(std::filesystem::file_size(journal) == clean_size, "journal tail was not repaired");

    const auto recovery = recover(root);
    require(recovery.found && recovery.generation == 1U, "authority changed after journal repair");
    cleanup(root);
}

void test_generation_protocol_never_falls_back_to_legacy_after_corruption() {
    const auto root = temp_root("generation-no-legacy-fallback");
    create_store(root);
    require(std::filesystem::exists(root / "manifest.zmd"), "legacy compatibility mirror is missing");

    const auto generation = zevryon::massivedoc::store_generation_path(root, 1U);
    {
        std::fstream stream(generation, std::ios::binary | std::ios::in | std::ios::out);
        require(static_cast<bool>(stream), "cannot open committed generation for corruption");
        char value = '\0';
        stream.read(&value, 1);
        require(static_cast<bool>(stream), "cannot read committed generation byte");
        value = static_cast<char>(value ^ 0x7f);
        stream.seekp(0);
        stream.write(&value, 1);
        require(static_cast<bool>(stream), "cannot corrupt committed generation byte");
    }

    zevryon::massivedoc::StoreReader reader(root);
    std::string error;
    require(!reader.open(&error), "reader silently fell back to legacy authority");
    require(
        error.find("no committed store generation") != std::string::npos,
        "corrupt generation failure was not diagnostic");
    require(
        std::filesystem::exists(root / "manifest.zmd"),
        "legacy mirror unexpectedly disappeared during rejection");
    cleanup(root);
}

} // namespace

int main() {
    test_committed_generation_is_reader_authority();
    test_uncommitted_generations_never_supersede_committed_authority();
    test_reader_repairs_corrupt_journal_tail();
    test_generation_protocol_never_falls_back_to_legacy_after_corruption();
    std::cout << "Zevryon MassiveDoc generation runtime tests passed\n";
    return 0;
}
