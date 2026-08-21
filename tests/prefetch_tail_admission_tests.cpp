#include "prefetch_tail_admission.hpp"

#include <cstddef>
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

void test_full_window_is_unchanged() {
    SourceWindowPrefetchResult result;
    result.succeeded = true;
    result.request.max_bytes = 64U * 1024U;
    result.bytes.resize(result.request.max_bytes);
    require(
        canonicalize_prefetch_tail_for_exact_admission(&result) ==
            PrefetchTailAdmissionResult::Unchanged,
        "full prefetch window was not preserved");
    require(result.request.max_bytes == 64U * 1024U,
            "full prefetch window request size changed");
}

void test_short_success_becomes_exact_tail() {
    SourceWindowPrefetchResult result;
    result.succeeded = true;
    result.request.max_bytes = 64U * 1024U;
    result.bytes.resize(123U);
    require(
        canonicalize_prefetch_tail_for_exact_admission(&result) ==
            PrefetchTailAdmissionResult::Canonicalized,
        "short successful prefetch was not canonicalized");
    require(result.request.max_bytes == 123U,
            "short successful prefetch did not become exact tail key");
}

void test_invalid_results_fail_closed() {
    SourceWindowPrefetchResult failed;
    failed.succeeded = false;
    failed.request.max_bytes = 64U * 1024U;
    failed.bytes.resize(10U);
    require(
        canonicalize_prefetch_tail_for_exact_admission(&failed) ==
            PrefetchTailAdmissionResult::Invalid,
        "failed result was canonicalized");

    SourceWindowPrefetchResult empty;
    empty.succeeded = true;
    empty.request.max_bytes = 64U * 1024U;
    require(
        canonicalize_prefetch_tail_for_exact_admission(&empty) ==
            PrefetchTailAdmissionResult::Invalid,
        "empty successful result was admitted as tail");

    SourceWindowPrefetchResult oversized;
    oversized.succeeded = true;
    oversized.request.max_bytes = 4U;
    oversized.bytes.resize(5U);
    require(
        canonicalize_prefetch_tail_for_exact_admission(&oversized) ==
            PrefetchTailAdmissionResult::Invalid,
        "oversized result was canonicalized");

    require(
        canonicalize_prefetch_tail_for_exact_admission(nullptr) ==
            PrefetchTailAdmissionResult::Invalid,
        "null result did not fail closed");
}

} // namespace

int main() {
    test_full_window_is_unchanged();
    test_short_success_becomes_exact_tail();
    test_invalid_results_fail_closed();
    std::cout << "Zevryon exact-tail prefetch admission tests passed\n";
    return 0;
}
