#include "massivedoc_store.hpp"
#include "shared_source_prefetch_pool.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace zevryon::massivedoc;
using namespace std::chrono_literals;

[[noreturn]] void die(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        die(message);
    }
}

SourceWindowPrefetchRequest make_request(
    std::uint64_t record_index,
    std::size_t max_bytes,
    PrefetchTicket ticket,
    std::uint64_t byte_offset = 0U) {
    SourceWindowPrefetchRequest request;
    request.record_index = record_index;
    request.byte_offset = byte_offset;
    request.max_bytes = max_bytes;
    request.ticket = ticket;
    return request;
}

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> output;
    output.reserve(text.size());
    for (const char value : text) {
        output.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return output;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    std::string output;
    output.reserve(bytes.size());
    for (const std::byte value : bytes) {
        output.push_back(
            static_cast<char>(std::to_integer<unsigned char>(value)));
    }
    return output;
}

struct CallRecord {
    std::uint64_t session_id{0U};
    std::uint64_t record_index{0U};
};

struct ExecutorGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool block_first{false};
    bool first_entered{false};
    bool release_first{false};
    std::vector<CallRecord> calls;
};

SharedSourcePrefetchExecutor recording_executor(
    const std::shared_ptr<ExecutorGate>& gate) {
    return [gate](
               const std::filesystem::path&,
               std::uint64_t session_id,
               const SourceWindowPrefetchRequest& request,
               std::vector<std::byte>* bytes,
               std::string* error) {
        if (bytes == nullptr || error == nullptr) {
            return false;
        }
        std::unique_lock<std::mutex> lock(gate->mutex);
        gate->calls.push_back(CallRecord{session_id, request.record_index});
        if (gate->block_first && gate->calls.size() == 1U) {
            gate->first_entered = true;
            gate->cv.notify_all();
            gate->cv.wait(lock, [gate] { return gate->release_first; });
        }
        lock.unlock();
        bytes->assign(
            request.max_bytes,
            static_cast<std::byte>(
                static_cast<unsigned char>(request.record_index & 0xffU)));
        error->clear();
        return true;
    };
}

void wait_first_entered(const std::shared_ptr<ExecutorGate>& gate) {
    std::unique_lock<std::mutex> lock(gate->mutex);
    require(
        gate->cv.wait_for(lock, 5s, [gate] { return gate->first_entered; }),
        "shared prefetch executor did not enter first blocked request");
}

void release_first(const std::shared_ptr<ExecutorGate>& gate) {
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->release_first = true;
    }
    gate->cv.notify_all();
}

void test_config_and_lazy_bounded_threads() {
    require(
        SharedSourcePrefetchPoolConfig{2U, 64U * 1024U}.valid(),
        "baseline shared pool config invalid");
    require(
        !SharedSourcePrefetchPoolConfig{0U, 64U * 1024U}.valid(),
        "zero-worker shared pool config accepted");
    require(
        !SharedSourcePrefetchPoolConfig{65U, 64U * 1024U}.valid(),
        "unbounded worker count accepted");

    SharedSourcePrefetchPool pool(
        {2U, 64U * 1024U},
        [](const std::filesystem::path&,
           std::uint64_t,
           const SourceWindowPrefetchRequest& request,
           std::vector<std::byte>* bytes,
           std::string* error) {
            bytes->assign(request.max_bytes, std::byte{0x2a});
            error->clear();
            return true;
        });
    const PrefetchTicket ticket{10U, 1};
    require(pool.open_session(1U, {}, ticket), "failed to open shared session");
    require(pool.status().live_threads == 0U, "pool eagerly started native threads");

    require(
        pool.request(1U, make_request(0U, 0U, ticket)) ==
            SharedSourcePrefetchScheduleResult::invalid,
        "invalid request admitted by shared pool");
    require(
        pool.request(1U, make_request(0U, 1U, PrefetchTicket{10U, 0})) ==
            SharedSourcePrefetchScheduleResult::stale,
        "stationary request admitted by shared pool");
    require(pool.status().live_threads == 0U,
            "invalid/stale traffic started shared worker threads");

    require(
        pool.request(1U, make_request(0U, 8U, ticket)) ==
            SharedSourcePrefetchScheduleResult::accepted,
        "first valid shared request rejected");
    require(pool.wait_idle_for(5s), "shared pool did not become idle");
    const SharedSourcePrefetchPoolStatus status = pool.status();
    require(status.live_threads == 2U, "shared pool did not hold configured thread bound");
    require(status.thread_starts == 2U, "shared pool thread-start count mismatch");
    require(status.sessions == 1U, "shared pool session count mismatch");
    require(status.runs_started == 1U, "shared pool run count mismatch");

    SourceWindowPrefetchResult result;
    require(pool.try_take_ready(1U, &result), "shared pool result missing");
    require(result.succeeded && result.bytes.size() == 8U,
            "shared pool result payload invalid");
}

void test_round_robin_latest_pending_fairness() {
    const auto gate = std::make_shared<ExecutorGate>();
    gate->block_first = true;
    SharedSourcePrefetchPool pool(
        {1U, 64U * 1024U},
        recording_executor(gate));
    const PrefetchTicket first_ticket{20U, 1};
    const PrefetchTicket second_ticket{30U, -1};
    require(pool.open_session(1U, {}, first_ticket), "failed to open fairness session one");
    require(pool.open_session(2U, {}, second_ticket), "failed to open fairness session two");

    require(
        pool.request(1U, make_request(1U, 4U, first_ticket)) ==
            SharedSourcePrefetchScheduleResult::accepted,
        "fairness first request rejected");
    wait_first_entered(gate);
    require(
        pool.request(1U, make_request(2U, 4U, first_ticket)) ==
            SharedSourcePrefetchScheduleResult::accepted,
        "fairness pending request rejected");
    require(
        pool.request(1U, make_request(3U, 4U, first_ticket)) ==
            SharedSourcePrefetchScheduleResult::replaced,
        "latest pending request did not replace prior same-session work");
    require(
        pool.request(2U, make_request(4U, 4U, second_ticket)) ==
            SharedSourcePrefetchScheduleResult::accepted,
        "second session fairness request rejected");

    release_first(gate);
    require(pool.wait_idle_for(5s), "fairness pool did not become idle");
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        require(gate->calls.size() == 3U, "fairness executor call count mismatch");
        require(gate->calls[0].session_id == 1U && gate->calls[0].record_index == 1U,
                "fairness first execution mismatch");
        require(gate->calls[1].session_id == 2U && gate->calls[1].record_index == 4U,
                "second session did not run before requeued noisy session");
        require(gate->calls[2].session_id == 1U && gate->calls[2].record_index == 3U,
                "latest same-session request was not requeued last");
    }
    const SharedSourcePrefetchPoolStatus status = pool.status();
    require(status.fairness_requeues == 1U, "fairness requeue was not counted");
    require(status.requests_replaced == 1U, "pending replacement count mismatch");
    require(status.ready_replacements == 1U,
            "same-session ready result was not bounded to one slot");
}

void test_hidden_inactive_cancellation_and_resume() {
    const auto gate = std::make_shared<ExecutorGate>();
    gate->block_first = true;
    SharedSourcePrefetchPool pool(
        {1U, 64U * 1024U},
        recording_executor(gate));
    const PrefetchTicket forward{40U, 1};
    require(pool.open_session(7U, {}, forward), "failed to open hidden-tab session");
    require(
        pool.request(7U, make_request(5U, 8U, forward)) ==
            SharedSourcePrefetchScheduleResult::accepted,
        "hidden-tab running request rejected");
    wait_first_entered(gate);

    const PrefetchTicket hidden_ticket{41U, 0};
    require(pool.set_session_authority(7U, hidden_ticket, false),
            "failed to mark session inactive");
    require(
        pool.request(7U, make_request(6U, 8U, hidden_ticket)) ==
            SharedSourcePrefetchScheduleResult::inactive,
        "inactive session admitted speculative work");
    release_first(gate);
    require(pool.wait_idle_for(5s), "inactive session did not settle");

    SourceWindowPrefetchResult result;
    require(!pool.try_take_ready(7U, &result),
            "running result survived hidden/inactive transition");
    require(pool.status().inactive_results_dropped == 1U,
            "hidden running result drop was not counted");

    const PrefetchTicket resumed{42U, -1};
    require(pool.set_session_authority(7U, resumed, true),
            "failed to resume shared session");
    require(
        pool.request(7U, make_request(7U, 6U, resumed)) ==
            SharedSourcePrefetchScheduleResult::accepted,
        "resumed shared session rejected fresh prefetch");
    require(pool.wait_idle_for(5s), "resumed session did not settle");
    require(pool.try_take_ready(7U, &result), "resumed session result missing");
    require(result.request.record_index == 7U,
            "resumed session published wrong request identity");
}

void test_global_ready_memory_budget() {
    SharedSourcePrefetchPool pool(
        {1U, 4U},
        [](const std::filesystem::path&,
           std::uint64_t,
           const SourceWindowPrefetchRequest& request,
           std::vector<std::byte>* bytes,
           std::string* error) {
            bytes->assign(request.max_bytes, std::byte{0x5a});
            error->clear();
            return true;
        });
    const PrefetchTicket first{50U, 1};
    const PrefetchTicket second{51U, -1};
    require(pool.open_session(1U, {}, first), "ready-budget first session open failed");
    require(pool.open_session(2U, {}, second), "ready-budget second session open failed");
    require(
        pool.request(1U, make_request(1U, 4U, first)) ==
            SharedSourcePrefetchScheduleResult::accepted,
        "ready-budget first request rejected");
    require(
        pool.request(2U, make_request(2U, 4U, second)) ==
            SharedSourcePrefetchScheduleResult::accepted,
        "ready-budget second request rejected");
    require(pool.wait_idle_for(5s), "ready-budget pool did not become idle");

    const SharedSourcePrefetchPoolStatus status = pool.status();
    require(status.ready_bytes <= 4U, "ready-result memory exceeded hard pool budget");
    require(status.ready_results == 1U, "ready-result hard budget retained too many results");
    require(status.ready_budget_drops == 1U,
            "ready-result budget pressure did not drop speculative result");
}

void test_registry_identity_close_and_reopen() {
    SharedSourcePrefetchPool pool({1U, 64U * 1024U});
    const PrefetchTicket ticket{60U, 1};
    require(pool.open_session(1U, {}, ticket), "registry first open failed");
    require(pool.open_session(2U, {}, ticket), "registry second open failed");
    require(pool.open_session(3U, {}, ticket), "registry third open failed");
    require(pool.open_session(4U, {}, ticket), "registry fourth open failed");
    require(!pool.open_session(3U, {}, ticket), "duplicate session identity admitted");
    require(pool.close_session(1U), "session close failed");
    require(pool.open_session(1U, {}, ticket), "closed session identity was not reusable");
    require(!pool.close_session(99U), "unknown session was reported closed");
}

void test_real_store_shared_prefetch() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "zevryon-shared-source-prefetch-tests";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    const std::vector<std::byte> payload = bytes_of("abcdefghijklmnopqrstuvwxyz");
    {
        StoreWriter writer(root);
        std::string error;
        require(writer.append(77U, std::span<const std::byte>(payload), &error),
                "shared real-store append failed");
        CorpusMetadata metadata;
        metadata.logical_utf8_bytes = payload.size();
        metadata.logical_records = 1U;
        metadata.logical_nodes = 1U;
        StoreStats stats;
        require(writer.finalize(metadata, &stats, &error),
                "shared real-store finalize failed");
    }

    {
        SharedSourcePrefetchPool pool({1U, 64U * 1024U});
        const PrefetchTicket ticket{70U, 1};
        require(pool.open_session(9U, root, ticket), "shared real-store session open failed");
        require(
            pool.request(9U, make_request(0U, 5U, ticket, 3U)) ==
                SharedSourcePrefetchScheduleResult::accepted,
            "shared real-store request rejected");
        require(pool.wait_idle_for(5s), "shared real-store request did not settle");
        SourceWindowPrefetchResult result;
        require(pool.try_take_ready(9U, &result), "shared real-store result missing");
        require(result.succeeded && result.error.empty(),
                "shared real-store result failed");
        require(text_of(result.bytes) == "defgh", "shared real-store bytes mismatch");
    }

    std::filesystem::remove_all(root, ignored);
}

} // namespace

int main() {
    test_config_and_lazy_bounded_threads();
    test_round_robin_latest_pending_fairness();
    test_hidden_inactive_cancellation_and_resume();
    test_global_ready_memory_budget();
    test_registry_identity_close_and_reopen();
    test_real_store_shared_prefetch();
    std::cout << "Zevryon shared source-prefetch pool tests passed\n";
    return 0;
}
