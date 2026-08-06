#include "massivedoc_descriptor_shadow.hpp"
#include "massivedoc_store.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "MassiveDoc descriptor shadow failure: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

std::vector<std::byte> make_payload(std::size_t bytes, std::uint8_t seed) {
    std::vector<std::byte> payload(bytes);
    std::uint8_t value = seed;
    for (std::byte& byte : payload) {
        value = static_cast<std::uint8_t>(value * 33U + 17U);
        byte = static_cast<std::byte>(value);
    }
    return payload;
}

void run_positive() {
    namespace md = zevryon::massivedoc;
    md::reset_massivedoc_descriptor_shadow();

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "zevryon-z2r3c-codec-shadow";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);

    md::StoreConfig config;
    config.segment_bytes = 32U;
    config.records_per_search_block = 2U;
    md::StoreWriter writer(root, config);
    std::string error;
    const auto first = make_payload(97U, 7U);
    const auto second = make_payload(65U, 19U);
    require(writer.append(101U, first, &error), error);
    require(writer.append(202U, second, &error), error);

    md::CorpusMetadata metadata;
    metadata.logical_nodes = 11U;
    md::StoreStats stats;
    require(writer.finalize(metadata, &stats, &error), error);
    require(stats.corpus.logical_records == 2U, "record count diverged");
    require(stats.chunk_count >= 6U, "fixture did not cross segment boundaries");

    md::StoreReader reader(root);
    require(reader.open(&error), error);
    require(reader.verify(&error), error);
    std::vector<std::byte> slice;
    require(reader.read_record_slice(0U, 29U, 50U, &slice, &error), error);
    require(slice.size() == 50U, "bounded slice size diverged");

    const auto shadow = md::massivedoc_descriptor_shadow_snapshot();
    require(shadow.record_encode_checks == 2U, "record encode shadow count diverged");
    require(shadow.chunk_encode_checks == stats.chunk_count, "chunk encode shadow count diverged");
    require(shadow.record_decode_checks >= 3U, "record decode shadow did not observe reads");
    require(shadow.chunk_decode_checks >= stats.chunk_count, "chunk decode shadow did not observe reads");
    require(shadow.mismatches == 0U, "production descriptor shadow mismatch");
    require(
        shadow.first_mismatch == md::MassiveDocDescriptorShadowOperation::None,
        "unexpected first mismatch operation");

    std::filesystem::remove_all(root, cleanup_error);
}

void run_fault() {
    namespace md = zevryon::massivedoc;
    md::reset_massivedoc_descriptor_shadow();
    const std::vector<std::byte> wrong_bytes(32U, std::byte{0});
    md::verify_massivedoc_record_encoding(
        1U, 2U, 3U, 4U, 5U, std::span<const std::byte>(wrong_bytes));
    const auto shadow = md::massivedoc_descriptor_shadow_snapshot();
    require(shadow.record_encode_checks == 1U, "fault encode check not counted");
    require(shadow.mismatches == 1U, "fault mismatch not detected");
    require(
        shadow.first_mismatch == md::MassiveDocDescriptorShadowOperation::RecordEncode,
        "fault operation was not latched");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--fault") {
        run_fault();
    } else {
        run_positive();
    }
    std::cout << "MassiveDoc descriptor shadow certification passed\n";
    return 0;
}
