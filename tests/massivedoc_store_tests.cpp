#include "massivedoc_store.hpp"

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
    std::mt19937_64 random(0x5a455652594f4eULL);
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("zevryon-") + std::string(name) + "-" + std::to_string(random()));
    std::filesystem::remove_all(root);
    return root;
}

std::vector<std::byte> bytes_of(const std::string& text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(begin, begin + text.size());
}

void test_roundtrip_search_and_integrity() {
    const auto root = temp_root("store");
    zevryon::massivedoc::StoreConfig config;
    config.segment_bytes = 4096U;
    config.records_per_search_block = 8U;
    zevryon::massivedoc::StoreWriter writer(root, config);
    std::string expected;
    std::string error;
    for (std::uint64_t index = 0U; index < 100U; ++index) {
        std::string record = "record-" + std::to_string(index) + " payload İstanbul العربية 漢字 ";
        if (index == 99U) {
            record += "ZEVRYON_TAIL_MARKER";
        }
        expected += record;
        const auto bytes = bytes_of(record);
        require(writer.append(index + 1000U, bytes, &error), error);
    }
    zevryon::massivedoc::CorpusMetadata metadata;
    metadata.logical_nodes = 800U;
    metadata.style_runs = 400U;
    metadata.resource_references = 10U;
    zevryon::massivedoc::StoreStats written;
    require(writer.finalize(metadata, &written, &error), error);
    require(written.corpus.logical_records == 100U, "record count mismatch");
    require(written.segment_count > 1U, "small segment test did not rotate files");

    zevryon::massivedoc::StoreReader reader(root);
    require(reader.open(&error), error);
    require(reader.verify(&error), error);
    const auto hits = reader.find("ZEVRYON_TAIL_MARKER", 10U, &error);
    require(error.empty(), error);
    require(hits.size() == 1U, "tail marker search result count");
    require(hits[0].record_index == 99U && hits[0].logical_id == 1099U, "tail marker hit identity");

    std::vector<std::byte> slice;
    require(reader.read_record_slice(99U, hits[0].byte_offset, 21U, &slice, &error), error);
    const std::string slice_text(reinterpret_cast<const char*>(slice.data()), slice.size());
    require(slice_text == "ZEVRYON_TAIL_MARKER", "record slice materialization failed");

    const auto output = root / "export.bin";
    require(reader.export_payload(output, &error), error);
    std::ifstream stream(output, std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    require(actual == expected, "export differs from source payload");
    std::filesystem::remove_all(root);
}

void test_giant_record_is_segmented_without_loss() {
    const auto root = temp_root("giant");
    zevryon::massivedoc::StoreConfig config;
    config.segment_bytes = 1024U * 1024U;
    config.records_per_search_block = 4U;
    zevryon::massivedoc::StoreWriter writer(root, config);
    std::string giant(5U * 1024U * 1024U + 123U, 'x');
    giant.replace(giant.size() - 32U, 21U, "GIANT_RECORD_MARKER!!");
    const auto bytes = bytes_of(giant);
    std::string error;
    require(writer.append(7U, bytes, &error), error);
    zevryon::massivedoc::StoreStats stats;
    require(writer.finalize({}, &stats, &error), error);
    require(stats.segment_count >= 6U, "giant record was not split across segments");
    require(stats.chunk_count >= 6U, "giant record chunk count too small");

    zevryon::massivedoc::StoreReader reader(root);
    require(reader.open(&error), error);
    require(reader.verify(&error), error);
    const auto hits = reader.find("GIANT_RECORD_MARKER!!", 1U, &error);
    require(error.empty() && hits.size() == 1U, "giant record marker search failed");
    std::filesystem::remove_all(root);
}

void test_corruption_is_detected() {
    const auto root = temp_root("corrupt");
    zevryon::massivedoc::StoreWriter writer(root, {});
    std::string error;
    const auto payload = bytes_of("corruption sentinel payload");
    require(writer.append(1U, payload, &error), error);
    zevryon::massivedoc::StoreStats stats;
    require(writer.finalize({}, &stats, &error), error);

    const auto segment = root / "segments" / "segment-00000000.bin";
    std::fstream stream(segment, std::ios::binary | std::ios::in | std::ios::out);
    require(static_cast<bool>(stream), "cannot open segment for corruption test");
    char byte = '\0';
    stream.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0x5a);
    stream.seekp(0);
    stream.write(&byte, 1);
    stream.close();

    zevryon::massivedoc::StoreReader reader(root);
    require(reader.open(&error), error);
    error.clear();
    require(!reader.verify(&error), "corrupted store was accepted");
    require(error.find("integrity") != std::string::npos || error.find("SHA") != std::string::npos,
            "corruption error was not diagnostic");
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    test_roundtrip_search_and_integrity();
    test_giant_record_is_segmented_without_loss();
    test_corruption_is_detected();
    std::cout << "Zevryon MassiveDoc store tests passed\n";
    return 0;
}
