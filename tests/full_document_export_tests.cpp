#include "full_document_export.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace zevryon::massivedoc;

[[noreturn]] void die(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        die(message);
    }
}

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(begin, begin + text.size());
}

std::filesystem::path test_root(std::string_view name) {
    const auto root = std::filesystem::temp_directory_path() /
        ("zevryon-full-export-" + std::string(name));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    require(!ignored, "cannot clean test root");
    std::filesystem::create_directories(root, ignored);
    require(!ignored, "cannot create test root");
    return root;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "cannot read output file");
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(stream), "cannot create sentinel file");
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(stream), "cannot write sentinel file");
}

void append_text(StoreWriter* writer, std::uint64_t id, std::string_view text) {
    std::string error;
    const auto payload = bytes_of(text);
    require(writer->append(id, payload, &error), "append text record");
}

void build_three_record_store(const std::filesystem::path& store_root) {
    StoreWriter writer(store_root);
    append_text(&writer, 10U, "first&");
    append_text(&writer, 20U, "<second>\"");
    append_text(&writer, 30U, "third");
    CorpusMetadata metadata;
    metadata.logical_records = 3U;
    metadata.logical_utf8_bytes = 6U + 9U + 5U;
    metadata.largest_record_bytes = 9U;
    StoreStats stats;
    std::string error;
    require(writer.finalize(metadata, &stats, &error), "finalize three-record store");
}

ChunkedOrderStatisticsSequence build_logical_order() {
    ChunkedOrderStatisticsSequence sequence(2U);
    std::string error;
    require(
        sequence.insert(0U, SequenceRecord{30U, 5U, 256U, 0U, 2U}, &error),
        "insert third logical record");
    require(
        sequence.insert(1U, SequenceRecord{10U, 6U, 256U, 0U, 0U}, &error),
        "insert first logical record");
    require(
        sequence.insert(2U, SequenceRecord{20U, 9U, 256U, 0U, 1U}, &error),
        "insert second logical record");
    return sequence;
}

void test_logical_order_text_and_html() {
    const auto root = test_root("logical");
    const auto store_root = root / "store";
    build_three_record_store(store_root);
    StoreReader reader(store_root);
    std::string error;
    require(reader.open(&error), "open three-record store");

    auto sequence = build_logical_order();
    const auto selection = full_document_selection(sequence.snapshot());
    require(sequence.move(0U, 2U, &error), "mutate live order after snapshot");

    FullDocumentExportStats text_stats;
    const auto text_output = root / "document.txt";
    require(
        export_full_document(
            reader,
            selection,
            text_output,
            FullDocumentExportFormat::Text,
            [] { return false; },
            {},
            &text_stats,
            &error),
        "text export failed");
    require(read_text(text_output) == "thirdfirst&<second>\"", "logical text export order");
    require(text_stats.records_exported == 3U, "text records exported");
    require(text_stats.source_bytes == 20U, "text source bytes");
    require(text_stats.output_bytes == 20U, "text output bytes");

    FullDocumentExportStats html_stats;
    const auto html_output = root / "document.html";
    require(
        export_full_document(
            reader,
            selection,
            html_output,
            FullDocumentExportFormat::Html,
            [] { return false; },
            {},
            &html_stats,
            &error),
        "HTML export failed");
    require(
        read_text(html_output) ==
            "<!doctype html><html><head><meta charset=\"utf-8\"></head><body><pre>"
            "thirdfirst&amp;&lt;second&gt;&quot;"
            "</pre></body></html>",
        "escaped HTML export content");
    require(html_stats.records_exported == 3U, "HTML records exported");
    require(html_stats.source_bytes == 20U, "HTML source bytes");
    require(html_stats.output_bytes > html_stats.source_bytes, "HTML escaping must expand output");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    require(!ignored, "cleanup logical export root");
}

void test_fixed_memory_large_record() {
    const auto root = test_root("large");
    const auto store_root = root / "store";
    const std::string payload(256U * 1024U, 'a');
    StoreWriter writer(store_root);
    append_text(&writer, 1U, payload);
    CorpusMetadata metadata;
    metadata.logical_records = 1U;
    metadata.logical_utf8_bytes = payload.size();
    metadata.largest_record_bytes = payload.size();
    StoreStats store_stats;
    std::string error;
    require(writer.finalize(metadata, &store_stats, &error), "finalize large store");

    StoreReadConfig read_config;
    read_config.io_window_bytes = 64U * 1024U;
    StoreReader reader(store_root, read_config);
    require(reader.open(&error), "open large store");

    ChunkedOrderStatisticsSequence sequence;
    require(
        sequence.insert(
            0U,
            SequenceRecord{1U, payload.size(), 256U, 0U, 0U},
            &error),
        "insert large logical record");
    const auto selection = full_document_selection(sequence.snapshot());

    FullDocumentExportOptions options;
    options.cancellation_block_bytes = 4096U;
    FullDocumentExportStats stats;
    const auto output = root / "large.html";
    require(
        export_full_document(
            reader,
            selection,
            output,
            FullDocumentExportFormat::Html,
            [] { return false; },
            options,
            &stats,
            &error),
        "large HTML export failed");
    require(stats.records_exported == 1U, "large record count");
    require(stats.source_bytes == payload.size(), "large source byte count");
    require(stats.peak_input_block_bytes <= 4096U, "input block exceeded fixed bound");
    require(stats.peak_output_buffer_bytes <= 32U * 1024U, "HTML output buffer exceeded bound");
    require(stats.peak_validation_codepoints <= 4096U + 1U, "UTF-8 validation output exceeded bound");
    require(std::filesystem::file_size(output) == stats.output_bytes, "large output byte accounting");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    require(!ignored, "cleanup large export root");
}

void test_cancellation_preserves_existing_target() {
    const auto root = test_root("cancel");
    const auto store_root = root / "store";
    build_three_record_store(store_root);
    StoreReader reader(store_root);
    std::string error;
    require(reader.open(&error), "open cancellation store");
    auto sequence = build_logical_order();
    const auto selection = full_document_selection(sequence.snapshot());

    const auto output = root / "target.txt";
    write_text(output, "sentinel");
    std::uint64_t checks = 0U;
    FullDocumentExportStats stats;
    require(
        !export_full_document(
            reader,
            selection,
            output,
            FullDocumentExportFormat::Text,
            [&checks] {
                ++checks;
                return checks >= 3U;
            },
            {},
            &stats,
            &error),
        "cancelled export must fail");
    require(stats.cancelled, "cancelled export stats");
    require(error == "export cancelled", "cancelled export error");
    require(read_text(output) == "sentinel", "cancelled export replaced existing target");
    auto temporary = output;
    temporary += ".zevryon-tmp";
    require(!std::filesystem::exists(temporary), "cancelled export left temporary file");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    require(!ignored, "cleanup cancellation root");
}

void test_invalid_utf8_html_fails_but_text_preserves_bytes() {
    const auto root = test_root("invalid");
    const auto store_root = root / "store";
    StoreWriter writer(store_root);
    const std::array<std::byte, 3> invalid{
        std::byte{static_cast<unsigned char>('a')},
        std::byte{static_cast<unsigned char>(0xffU)},
        std::byte{static_cast<unsigned char>('b')}};
    std::string error;
    require(writer.append(1U, invalid, &error), "append invalid UTF-8 record");
    CorpusMetadata metadata;
    metadata.logical_records = 1U;
    metadata.logical_utf8_bytes = 3U;
    metadata.largest_record_bytes = 3U;
    StoreStats store_stats;
    require(writer.finalize(metadata, &store_stats, &error), "finalize invalid store");
    StoreReader reader(store_root);
    require(reader.open(&error), "open invalid store");

    ChunkedOrderStatisticsSequence sequence;
    require(
        sequence.insert(0U, SequenceRecord{1U, 3U, 256U, 0U, 0U}, &error),
        "insert invalid logical record");
    const auto selection = full_document_selection(sequence.snapshot());

    const auto html_output = root / "invalid.html";
    write_text(html_output, "sentinel");
    FullDocumentExportStats stats;
    require(
        !export_full_document(
            reader,
            selection,
            html_output,
            FullDocumentExportFormat::Html,
            [] { return false; },
            {},
            &stats,
            &error),
        "invalid UTF-8 HTML export must fail");
    require(error.find("valid UTF-8") != std::string::npos, "invalid UTF-8 HTML error");
    require(read_text(html_output) == "sentinel", "invalid HTML export replaced target");

    const auto text_output = root / "invalid.txt";
    require(
        export_full_document(
            reader,
            selection,
            text_output,
            FullDocumentExportFormat::Text,
            [] { return false; },
            {},
            &stats,
            &error),
        "raw text export should preserve source bytes");
    const std::string raw = read_text(text_output);
    require(raw.size() == invalid.size(), "raw text byte size");
    require(static_cast<unsigned char>(raw[1]) == 0xffU, "raw text invalid byte preservation");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    require(!ignored, "cleanup invalid root");
}

void test_descriptor_metadata_mismatch_fails_before_publish() {
    const auto root = test_root("metadata");
    const auto store_root = root / "store";
    build_three_record_store(store_root);
    StoreReader reader(store_root);
    std::string error;
    require(reader.open(&error), "open metadata store");
    auto sequence = build_logical_order();
    auto selection = full_document_selection(sequence.snapshot());
    --selection.end_text_offset;

    const auto output = root / "target.txt";
    write_text(output, "sentinel");
    FullDocumentExportStats stats;
    require(
        !export_full_document(
            reader,
            selection,
            output,
            FullDocumentExportFormat::Text,
            [] { return false; },
            {},
            &stats,
            &error),
        "metadata mismatch export must fail");
    require(error.find("does not cover") != std::string::npos, "metadata mismatch error");
    require(read_text(output) == "sentinel", "metadata mismatch replaced target");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    require(!ignored, "cleanup metadata root");
}

} // namespace

int main() {
    test_logical_order_text_and_html();
    test_fixed_memory_large_record();
    test_cancellation_preserves_existing_target();
    test_invalid_utf8_html_fails_but_text_preserves_bytes();
    test_descriptor_metadata_mismatch_fails_before_publish();
    std::cout << "Zevryon full-document export tests passed\n";
    return 0;
}
