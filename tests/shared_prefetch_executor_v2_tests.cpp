#include "massivedoc_store.hpp"
#include "shared_record_length_authority.hpp"
#include "shared_source_prefetch_pool.hpp"
#include "store_record_length_probe.hpp"

#include <chrono>
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
using namespace zevryon::massivedoc;
using namespace std::chrono_literals;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        fail(message);
    }
}

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> output;
    output.reserve(text.size());
    for (const char value : text) {
        output.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(value)));
    }
    return output;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    std::string output;
    output.reserve(bytes.size());
    for (const std::byte value : bytes) {
        output.push_back(static_cast<char>(
            std::to_integer<unsigned char>(value)));
    }
    return output;
}

SourceWindowPrefetchRequest request_for(
    PrefetchTicket ticket,
    std::uint64_t offset,
    std::size_t bytes,
    std::uint64_t visible_edge) {
    SourceWindowPrefetchRequest request;
    request.record_index = 0U;
    request.byte_offset = offset;
    request.max_bytes = bytes;
    request.ticket = ticket;
    request.visible_edge_offset = visible_edge;
    request.has_visible_edge_offset = true;
    return request;
}

void test_worker_cold_resolve_canonicalizes_and_suppresses() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "zevryon-prefetch-executor-v2-real-store";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    const std::vector<std::byte> payload =
        bytes_of("abcdefghijklmnopqrstuvwxyz");
    {
        StoreWriter writer(root);
        std::string error;
        require(writer.append(77U, std::span<const std::byte>(payload), &error),
                "real-store append failed");
        CorpusMetadata metadata;
        metadata.logical_utf8_bytes = payload.size();
        metadata.logical_records = 1U;
        metadata.logical_nodes = 1U;
        metadata.largest_record_bytes = payload.size();
        StoreStats stats;
        require(writer.finalize(metadata, &stats, &error),
                "real-store finalize failed");
    }

    std::uint64_t probed = 0U;
    std::string probe_error;
    require(probe_store_record_length(root, 0U, &probed, &probe_error),
            "direct record-length probe failed");
    require(probed == payload.size(), "record-length probe returned wrong value");

    SharedRecordLengthAuthority authority;
    SharedSourcePrefetchPoolConfig config;
    config.worker_count = 1U;
    config.max_ready_bytes = 64U * 1024U;
    config.record_length_authority = &authority;
    SharedSourcePrefetchPool pool(config);

    const PrefetchTicket first{100U, 1};
    require(pool.open_session(1U, root, first), "first session open failed");
    require(
        pool.request(1U, request_for(first, 1024U, 8U, 20U)) ==
            SharedSourcePrefetchScheduleResult::accepted,
        "cold canonical request was rejected");
    require(pool.wait_idle_for(5s), "cold canonical request did not settle");

    SourceWindowPrefetchResult result;
    require(pool.try_take_ready(1U, &result), "canonical ready result missing");
    require(result.succeeded && result.error.empty(),
            "canonical ready result failed");
    require(result.request.byte_offset == 20U && result.request.max_bytes == 6U,
            "worker did not canonicalize request to exact record tail");
    require(text_of(result.bytes) == "uvwxyz", "canonical tail payload mismatch");

    const auto first_status = pool.status();
    require(first_status.canonicalized_results == 1U,
            "canonicalized result telemetry mismatch");
    require(first_status.live_threads == 1U,
            "worker count changed while resolving record metadata");
    require(authority.status().entries == 1U,
            "cold record length was not retained in shared authority");

    const PrefetchTicket second{101U, 1};
    require(pool.open_session(2U, root, second), "second session open failed");
    require(
        pool.request(2U, request_for(second, 26U, 8U, 26U)) ==
            SharedSourcePrefetchScheduleResult::accepted,
        "EOF suppression request was rejected before worker evaluation");
    require(pool.wait_idle_for(5s), "EOF suppression request did not settle");
    require(!pool.try_take_ready(2U, &result),
            "EOF-suppressed worker published an empty ready result");

    const auto second_status = pool.status();
    require(second_status.worker_eof_suppressions == 1U &&
                second_status.runs_suppressed == 1U,
            "worker EOF suppression telemetry mismatch");
    require(authority.status().cache_hits >= 1U,
            "second session did not reuse process-shared record metadata");

    std::filesystem::remove_all(root, ignored);
}

void test_executor_v2_publishes_canonical_request_identity() {
    SharedSourcePrefetchPool pool(
        {1U, 64U * 1024U},
        SharedSourcePrefetchExecutorV2{
            [](const std::filesystem::path&,
               std::uint64_t,
               const SourceWindowPrefetchRequest& request,
               SharedSourcePrefetchExecution* execution,
               std::string* error) {
                execution->canonical_request = request;
                execution->canonical_request.byte_offset = 5U;
                execution->canonical_request.max_bytes = 3U;
                execution->bytes.assign(3U, std::byte{0x2a});
                error->clear();
                return true;
            }});
    const PrefetchTicket ticket{200U, 1};
    require(pool.open_session(9U, {}, ticket), "V2 session open failed");
    SourceWindowPrefetchRequest request;
    request.record_index = 7U;
    request.byte_offset = 99U;
    request.max_bytes = 8U;
    request.ticket = ticket;
    require(
        pool.request(9U, request) == SharedSourcePrefetchScheduleResult::accepted,
        "V2 request rejected");
    require(pool.wait_idle_for(5s), "V2 request did not settle");
    SourceWindowPrefetchResult result;
    require(pool.try_take_ready(9U, &result), "V2 result missing");
    require(result.succeeded && result.request.record_index == 7U &&
                result.request.ticket == ticket &&
                result.request.byte_offset == 5U &&
                result.request.max_bytes == 3U,
            "V2 canonical request was not published with payload");
}

void test_executor_v2_rejects_identity_rewrite() {
    SharedSourcePrefetchPool pool(
        {1U, 64U * 1024U},
        SharedSourcePrefetchExecutorV2{
            [](const std::filesystem::path&,
               std::uint64_t,
               const SourceWindowPrefetchRequest& request,
               SharedSourcePrefetchExecution* execution,
               std::string* error) {
                execution->canonical_request = request;
                execution->canonical_request.record_index += 1U;
                execution->bytes.assign(1U, std::byte{0x01});
                error->clear();
                return true;
            }});
    const PrefetchTicket ticket{300U, -1};
    require(pool.open_session(11U, {}, ticket), "invalid-V2 session open failed");
    SourceWindowPrefetchRequest request;
    request.record_index = 3U;
    request.byte_offset = 1U;
    request.max_bytes = 4U;
    request.ticket = ticket;
    require(
        pool.request(11U, request) == SharedSourcePrefetchScheduleResult::accepted,
        "invalid-V2 request rejected before execution");
    require(pool.wait_idle_for(5s), "invalid-V2 request did not settle");
    SourceWindowPrefetchResult result;
    require(pool.try_take_ready(11U, &result), "invalid-V2 failure result missing");
    require(!result.succeeded && result.request.record_index == 3U &&
                result.error.find("invalid canonical request") != std::string::npos,
            "identity rewrite was not rejected fail-closed");
}

} // namespace

int main() {
    test_worker_cold_resolve_canonicalizes_and_suppresses();
    test_executor_v2_publishes_canonical_request_identity();
    test_executor_v2_rejects_identity_rewrite();
    std::cout << "Zevryon shared prefetch executor V2 tests passed\n";
    return 0;
}
