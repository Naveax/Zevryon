#include "massivedoc_generation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
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

constexpr int kInjectedCrashExitCode = 86;

bool parse_u64(std::string_view text, std::uint64_t* value) {
    if (value == nullptr || text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *value = parsed;
    return true;
}

bool write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(stream);
}

std::array<std::uint8_t, 32> identity_for(std::uint64_t generation) {
    std::array<std::uint8_t, 32> identity{};
    for (std::size_t index = 0U; index < identity.size(); ++index) {
        identity[index] = static_cast<std::uint8_t>(
            (generation * 17U + static_cast<std::uint64_t>(index)) & 0xffU);
    }
    return identity;
}

std::vector<std::byte> authority_for(std::uint64_t generation) {
    std::vector<std::byte> authority(160U, std::byte{0});
    for (std::size_t index = 0U; index < authority.size(); ++index) {
        authority[index] = static_cast<std::byte>(static_cast<unsigned char>(
            (generation + static_cast<std::uint64_t>(index)) & 0xffU));
    }
    return authority;
}

std::vector<GenerationSegmentInventory> fixture_segments() {
    return {{0U, 7U}};
}

bool ensure_fixture(const std::filesystem::path& root, std::string* error) {
    std::error_code fs_error;
    std::filesystem::create_directories(root / "segments", fs_error);
    if (fs_error) {
        *error = "cannot create crash fixture directories: " + fs_error.message();
        return false;
    }
    if (!write_text(root / "records.idx", "records") ||
        !write_text(root / "chunks.idx", "chunks") ||
        !write_text(root / "search.bgm", "search") ||
        !write_text(root / "segments" / "segment-00000000.bin", "payload")) {
        *error = "cannot write crash fixture files";
        return false;
    }
    return true;
}

bool publish_generation(
    const std::filesystem::path& root,
    std::uint64_t generation,
    GenerationPublicationCut cut,
    std::string* error) {
    const auto authority = authority_for(generation);
    const auto identity = identity_for(generation);
    const auto segments = fixture_segments();
    return zevryon::massivedoc::publish_store_generation(
        root,
        generation,
        authority,
        identity,
        segments,
        cut,
        error);
}

GenerationPublicationCut publication_cut(std::string_view name, bool* valid) {
    *valid = true;
    if (name == "after-payload-flush") return GenerationPublicationCut::after_payload_flush;
    if (name == "after-prepare") return GenerationPublicationCut::after_prepare;
    if (name == "after-manifest-temp") return GenerationPublicationCut::after_manifest_temp;
    if (name == "after-manifest") return GenerationPublicationCut::after_manifest;
    if (name == "after-commit") return GenerationPublicationCut::after_commit;
    *valid = false;
    return GenerationPublicationCut::none;
}

GenerationCompactionCut compaction_cut(std::string_view name, bool* valid) {
    *valid = true;
    if (name == "after-journal-temp") return GenerationCompactionCut::after_journal_temp;
    if (name == "after-journal-replace") return GenerationCompactionCut::after_journal_replace;
    if (name == "after-stale-quarantine") return GenerationCompactionCut::after_stale_quarantine;
    *valid = false;
    return GenerationCompactionCut::none;
}

std::string identity_hex(const std::array<std::uint8_t, 32>& identity) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto value : identity) {
        stream << std::setw(2) << static_cast<unsigned int>(value);
    }
    return stream.str();
}

int command_seed(const std::filesystem::path& root, std::uint64_t count) {
    std::error_code fs_error;
    std::filesystem::remove_all(root, fs_error);
    if (fs_error) {
        std::cerr << "cannot clear seed root: " << fs_error.message() << '\n';
        return 2;
    }
    std::string error;
    if (!ensure_fixture(root, &error)) {
        std::cerr << error << '\n';
        return 2;
    }
    for (std::uint64_t generation = 1U; generation <= count; ++generation) {
        if (!publish_generation(root, generation, GenerationPublicationCut::none, &error)) {
            std::cerr << "seed publication failed: " << error << '\n';
            return 2;
        }
    }
    return 0;
}

int command_publish(const std::filesystem::path& root, std::uint64_t generation, GenerationPublicationCut cut, bool crash) {
    std::string error;
    if (!publish_generation(root, generation, cut, &error)) {
        std::cerr << "publication failed: " << error << '\n';
        return 2;
    }
    if (crash) std::_Exit(kInjectedCrashExitCode);
    return 0;
}

int command_compact(const std::filesystem::path& root, GenerationCompactionCut cut, bool crash) {
    GenerationCompactionResult result;
    std::string error;
    if (!zevryon::massivedoc::compact_store_generation_metadata(root, GenerationCompactionConfig{2U}, cut, &result, &error)) {
        std::cerr << "compaction failed: " << error << '\n';
        return 2;
    }
    if (crash) std::_Exit(kInjectedCrashExitCode);
    return 0;
}

int command_recover(const std::filesystem::path& root) {
    GenerationRecovery recovery;
    std::string error;
    if (!zevryon::massivedoc::recover_store_generation(root, &recovery, &error)) {
        std::cerr << "recovery failed: " << error << '\n';
        return 2;
    }
    std::cout << "{\"protocol_present\":" << (recovery.protocol_present ? "true" : "false")
              << ",\"found\":" << (recovery.found ? "true" : "false")
              << ",\"generation\":" << recovery.generation
              << ",\"identity_hex\":\"" << identity_hex(recovery.source_identity)
              << "\",\"authority_bytes\":" << recovery.authority_manifest.size()
              << ",\"authority_first\":";
    if (recovery.authority_manifest.empty()) {
        std::cout << "null";
    } else {
        std::cout << static_cast<unsigned int>(std::to_integer<unsigned char>(recovery.authority_manifest.front()));
    }
    std::cout << ",\"segments\":[";
    for (std::size_t index = 0U; index < recovery.segments.size(); ++index) {
        if (index != 0U) std::cout << ',';
        std::cout << "{\"id\":" << recovery.segments[index].segment_id
                  << ",\"bytes\":" << recovery.segments[index].byte_length << '}';
    }
    std::cout << "]}\n";
    return 0;
}

void usage() {
    std::cerr << "usage: m8_storage_crash_probe <seed|crash-publish|publish|crash-compact|compact|recover> ...\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    const std::string_view command(argv[1]);
    const std::filesystem::path root(argv[2]);

    if (command == "seed") {
        if (argc != 4) { usage(); return 2; }
        std::uint64_t count = 0U;
        if (!parse_u64(argv[3], &count) || count == 0U) {
            std::cerr << "invalid seed generation count\n";
            return 2;
        }
        return command_seed(root, count);
    }

    if (command == "recover") {
        if (argc != 3) { usage(); return 2; }
        return command_recover(root);
    }

    if (command == "crash-publish" || command == "publish") {
        if (argc != 5) { usage(); return 2; }
        std::uint64_t generation = 0U;
        if (!parse_u64(argv[3], &generation) || generation == 0U) {
            std::cerr << "invalid publication generation\n";
            return 2;
        }
        bool valid = false;
        const auto cut = command == "publish" ? GenerationPublicationCut::none : publication_cut(argv[4], &valid);
        if (command == "publish") valid = std::string_view(argv[4]) == "none";
        if (!valid) {
            std::cerr << "invalid publication cut\n";
            return 2;
        }
        return command_publish(root, generation, cut, command == "crash-publish");
    }

    if (command == "crash-compact" || command == "compact") {
        if (argc != 4) { usage(); return 2; }
        bool valid = false;
        const auto cut = command == "compact" ? GenerationCompactionCut::none : compaction_cut(argv[3], &valid);
        if (command == "compact") valid = std::string_view(argv[3]) == "none";
        if (!valid) {
            std::cerr << "invalid compaction cut\n";
            return 2;
        }
        return command_compact(root, cut, command == "crash-compact");
    }

    usage();
    return 2;
}
