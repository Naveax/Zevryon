#include "prefetch_record_bounds.hpp"
#include "shared_record_length_authority.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace zevryon::massivedoc;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        fail(message);
    }
}

void test_record_bound_prefetch() {
    auto decision = clamp_prefetch_to_record(1, 100U, 164U, 64U, 1000U);
    require(decision.should_issue && decision.byte_offset == 164U &&
                decision.request_bytes == 64U && !decision.offset_clamped,
            "forward request changed away from EOF");

    decision = clamp_prefetch_to_record(1, 100U, 900U, 64U, 300U);
    require(decision.should_issue && decision.byte_offset == 236U &&
                decision.request_bytes == 64U && decision.offset_clamped,
            "forward request did not clamp to last full window");

    decision = clamp_prefetch_to_record(1, 260U, 900U, 64U, 300U);
    require(decision.should_issue && decision.byte_offset == 260U &&
                decision.request_bytes == 40U && decision.bytes_clamped,
            "forward request did not become exact tail");

    decision = clamp_prefetch_to_record(1, 300U, 900U, 64U, 300U);
    require(!decision.should_issue && decision.eof_suppressed,
            "EOF edge still issued speculative work");

    decision = clamp_prefetch_to_record(-1, 40U, 0U, 64U, 300U);
    require(decision.should_issue && decision.byte_offset == 0U &&
                decision.request_bytes == 40U && decision.bytes_clamped,
            "reverse prefix was not exact");

    decision = clamp_prefetch_to_record(-1, 200U, 100U, 64U, 300U);
    require(decision.should_issue && decision.byte_offset == 100U &&
                decision.request_bytes == 64U,
            "reverse full window changed unexpectedly");

    decision = clamp_prefetch_to_record(1, 200U, 100U, 64U, 300U);
    require(!decision.should_issue,
            "forward prediction behind visible edge was accepted");
}

void test_shared_bounded_record_length_authority() {
    SharedRecordLengthAuthorityConfig config;
    config.max_entries = 2U;
    SharedRecordLengthAuthority authority(config);
    require(authority.valid(), "record-length authority config invalid");

    int resolves = 0;
    RecordLengthResolver resolver =
        [&resolves](
            const std::filesystem::path&,
            std::uint64_t record_index,
            std::uint64_t* length,
            std::string*) {
            ++resolves;
            *length = 1000U + record_index;
            return true;
        };

    std::uint64_t length = 0U;
    std::string error;
    require(authority.query("store-a", 1U, resolver, &length, &error) &&
                length == 1001U && resolves == 1,
            "first record-length resolution failed");
    require(authority.query("store-a", 1U, resolver, &length, &error) &&
                length == 1001U && resolves == 1,
            "second session-equivalent lookup missed shared cache");

    require(authority.query("store-a", 2U, resolver, &length, &error),
            "second bounded cache insert failed");
    require(authority.query("store-a", 3U, resolver, &length, &error),
            "third bounded cache insert failed");
    const auto bounded = authority.status();
    require(bounded.entries == 2U && bounded.evictions == 1U &&
                bounded.cache_hits >= 1U,
            "record-length authority did not remain bounded");

    require(authority.remember("store-a", 9U, 777U, &error),
            "learned EOF record length was not remembered");
    const int resolves_before_remembered_hit = resolves;
    require(authority.query("store-a", 9U, resolver, &length, &error) &&
                length == 777U && resolves == resolves_before_remembered_hit,
            "learned EOF record length did not become shared authority");

    require(authority.remember("store-b", 9U, 888U, &error),
            "second store identity was not admitted");
    require(authority.query("store-b", 9U, resolver, &length, &error) &&
                length == 888U,
            "store-root identity collided in record-length authority");
}

void test_invalid_and_resolver_failure_paths() {
    SharedRecordLengthAuthority authority;
    std::uint64_t length = 0U;
    std::string error;
    RecordLengthResolver failing = [](
                                      const std::filesystem::path&,
                                      std::uint64_t,
                                      std::uint64_t*,
                                      std::string* resolver_error) {
        *resolver_error = "expected resolver failure";
        return false;
    };
    require(!authority.query("store", 1U, failing, &length, &error) &&
                error == "expected resolver failure",
            "resolver failure did not fail closed");
    require(authority.status().resolver_failures == 1U,
            "resolver failure telemetry mismatch");

    SharedRecordLengthAuthorityConfig invalid;
    invalid.max_entries = 0U;
    SharedRecordLengthAuthority invalid_authority(invalid);
    require(!invalid_authority.valid(), "zero-entry authority accepted");
}

} // namespace

int main() {
    test_record_bound_prefetch();
    test_shared_bounded_record_length_authority();
    test_invalid_and_resolver_failure_paths();
    std::cout << "Zevryon shared record-length authority tests passed\n";
    return 0;
}
