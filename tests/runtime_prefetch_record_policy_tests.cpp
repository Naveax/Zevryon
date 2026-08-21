#include "runtime_prefetch_record_policy.hpp"
#include "shared_record_length_authority.hpp"
#include "zenith_tab_runtime_profile.hpp"

#include <cstdlib>
#include <iostream>
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

void test_cache_miss_never_blocks_or_changes_request() {
    SharedRecordLengthAuthority authority;
    const auto decision = apply_cached_record_bounds(
        &authority,
        "store",
        7U,
        1,
        260U,
        900U,
        64U);
    require(!decision.metadata_hit && decision.should_issue &&
                decision.byte_offset == 900U && decision.request_bytes == 64U,
            "metadata miss changed speculative request");
}

void test_short_result_teaches_future_tabs() {
    SharedRecordLengthAuthority authority;
    require(
        learn_record_length_from_short_prefetch(
            &authority,
            "store",
            7U,
            260U,
            64U,
            40U) == PrefetchRecordLengthLearnResult::Learned,
        "short EOF result was not learned");

    const auto exact_tail = apply_cached_record_bounds(
        &authority,
        "store",
        7U,
        1,
        260U,
        900U,
        64U);
    require(exact_tail.metadata_hit && exact_tail.should_issue &&
                exact_tail.byte_offset == 260U && exact_tail.request_bytes == 40U &&
                exact_tail.clamped,
            "learned record length did not produce exact tail");

    const auto eof = apply_cached_record_bounds(
        &authority,
        "store",
        7U,
        1,
        300U,
        900U,
        64U);
    require(eof.metadata_hit && !eof.should_issue && eof.eof_suppressed,
            "learned record length did not suppress EOF work");

    const auto reverse = apply_cached_record_bounds(
        &authority,
        "store",
        7U,
        -1,
        40U,
        0U,
        64U);
    require(reverse.metadata_hit && reverse.should_issue &&
                reverse.byte_offset == 0U && reverse.request_bytes == 40U,
            "learned record length did not bound reverse prefix");
}

void test_full_result_does_not_infer_eof() {
    SharedRecordLengthAuthority authority;
    require(
        learn_record_length_from_short_prefetch(
            &authority,
            "store",
            1U,
            0U,
            64U,
            64U) == PrefetchRecordLengthLearnResult::NotApplicable,
        "full result incorrectly inferred EOF");
    std::uint64_t length = 0U;
    require(!authority.try_get("store", 1U, &length),
            "full result populated record-length authority");
}

void test_profile_factory_shares_authority() {
    SharedRecordLengthAuthority authority;
    const ZenithTabRuntimeConfig config = make_zenith_tab_runtime_config(
        DeviceFrameProfile::Desktop,
        LayoutConfig{},
        &authority);
    require(config.record_length_authority == &authority,
            "device profile factory did not preserve process-shared authority");
    require(config.valid(), "authority wiring invalidated runtime config");
}

} // namespace

int main() {
    test_cache_miss_never_blocks_or_changes_request();
    test_short_result_teaches_future_tabs();
    test_full_result_does_not_infer_eof();
    test_profile_factory_shares_authority();
    std::cout << "Zevryon runtime record-length policy tests passed\n";
    return 0;
}
