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
    std::cerr << "MassiveDoc descriptor authority failure: " << message << '\n';
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

void require_authority_identity() {
    namespace md = zevryon::massivedoc;
    const auto snapshot = md::massivedoc_descriptor_shadow_snapshot();
    require(
        snapshot.authoritative_backend == md::MassiveDocDescriptorBackend::Rust,
        "public descriptor backend is not Rust");
    require(
        snapshot.verification_backend == md::MassiveDocDescriptorBackend::Cpp,
        "reverse-shadow backend is not C++");
}

void run_positive() {
    namespace md = zevryon::massivedoc;
    md::reset_massivedoc_descriptor_shadow();
    require_authority_identity();

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "zevryon-z2r3e-codec-authority";
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

    std::vector<std::byte> first_round_trip;
    require(reader.read_record(0U, &first_round_trip, &error), error);
    require(first_round_trip == first, "Rust-authoritative first record diverged");

    std::vector<std::byte> slice;
    require(reader.read_record_slice(1U, 13U, 41U, &slice, &error), error);
    require(slice.size() == 41U, "bounded slice size diverged");
    require(
        std::equal(slice.begin(), slice.end(), second.begin() + 13),
        "Rust-authoritative bounded slice diverged");

    const auto authority = md::massivedoc_descriptor_shadow_snapshot();
    require(authority.record_encode_checks == 2U, "record encode count diverged");
    require(
        authority.chunk_encode_checks == stats.chunk_count,
        "chunk encode count diverged");
    require(authority.record_decode_checks >= 4U, "record decode authority not exercised");
    require(
        authority.chunk_decode_checks >= stats.chunk_count,
        "chunk decode authority not exercised");
    require(authority.mismatches == 0U, "C++ reverse-shadow mismatch");
    require(
        authority.first_mismatch == md::MassiveDocDescriptorShadowOperation::None,
        "unexpected first mismatch operation");

    std::filesystem::remove_all(root, cleanup_error);
}

void run_fault() {
    namespace md = zevryon::massivedoc;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "zevryon-z2r3e-codec-authority-fault";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);

    const auto payload = make_payload(129U, 31U);
    md::StoreConfig config;
    config.segment_bytes = 40U;
    config.records_per_search_block = 1U;

    md::reset_massivedoc_descriptor_shadow();
    md::set_massivedoc_cpp_reverse_shadow_fault(
        md::MassiveDocDescriptorShadowOperation::RecordEncode);

    md::StoreWriter writer(root, config);
    std::string error;
    require(writer.append(404U, payload, &error), error);
    md::CorpusMetadata metadata;
    md::StoreStats stats;
    require(writer.finalize(metadata, &stats, &error), error);

    const auto encode_fault = md::massivedoc_descriptor_shadow_snapshot();
    require_authority_identity();
    require(encode_fault.record_encode_checks == 1U, "encode fault was not exercised");
    require(encode_fault.mismatches == 1U, "encode reverse-shadow fault not detected");
    require(
        encode_fault.first_mismatch ==
            md::MassiveDocDescriptorShadowOperation::RecordEncode,
        "encode reverse-shadow operation was not latched");

    md::clear_massivedoc_cpp_reverse_shadow_fault();
    md::reset_massivedoc_descriptor_shadow();

    md::StoreReader clean_reader(root);
    require(clean_reader.open(&error), error);
    require(clean_reader.verify(&error), error);
    std::vector<std::byte> clean_round_trip;
    require(clean_reader.read_record(0U, &clean_round_trip, &error), error);
    require(
        clean_round_trip == payload,
        "C++ encode reverse-shadow corruption changed Rust-authoritative disk bytes");

    md::reset_massivedoc_descriptor_shadow();
    md::set_massivedoc_cpp_reverse_shadow_fault(
        md::MassiveDocDescriptorShadowOperation::RecordDecode);

    md::StoreReader fault_reader(root);
    require(fault_reader.open(&error), error);
    require(fault_reader.verify(&error), error);
    std::vector<std::byte> fault_round_trip;
    require(fault_reader.read_record(0U, &fault_round_trip, &error), error);
    require(
        fault_round_trip == payload,
        "C++ decode reverse-shadow corruption changed Rust-authoritative output");

    const auto decode_fault = md::massivedoc_descriptor_shadow_snapshot();
    require_authority_identity();
    require(decode_fault.record_decode_checks > 0U, "decode fault was not exercised");
    require(decode_fault.mismatches > 0U, "decode reverse-shadow fault not detected");
    require(
        decode_fault.first_mismatch ==
            md::MassiveDocDescriptorShadowOperation::RecordDecode,
        "decode reverse-shadow operation was not latched");

    md::clear_massivedoc_cpp_reverse_shadow_fault();
    std::filesystem::remove_all(root, cleanup_error);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--fault") {
        run_fault();
    } else {
        run_positive();
    }
    std::cout << "MassiveDoc descriptor authority certification passed\n";
    return 0;
}
