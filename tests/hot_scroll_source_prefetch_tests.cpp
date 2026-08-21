#include "hot_scroll_source_prefetch.hpp"
#include "massivedoc_store.hpp"

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

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const unsigned char value : text) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const std::byte value : bytes) {
        text.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
    }
    return text;
}

SourceWindowPrefetchRequest make_request(
    std::uint64_t record_index,
    std::uint64_t byte_offset,
    std::size_t max_bytes,
    PrefetchTicket ticket) {
    SourceWindowPrefetchRequest request;
    request.record_index = record_index;
    request.byte_offset = byte_offset;
    request.max_bytes = max_bytes;
    request.ticket = ticket;
    return request;
}

struct ExecutorGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool first_entered{false};
    bool release_first{false};
    std::uint64_t calls{0U};
};

SourceWindowPrefetchExecutor gated_executor(const std::shared_ptr<ExecutorGate>& gate) {
    return [gate](
               const std::filesystem::path&,
               const SourceWindowPrefetchRequest& request,
               std::vector<std::byte>* bytes,
               std::string* error) {
        if (bytes == nullptr || error == nullptr) {
            return false;
        }
        std::unique_lock<std::mutex> lock(gate->mutex);
        ++gate->calls;
        if (gate->calls == 1U) {
            gate->first_entered = true;
            gate->cv.notify_all();
            gate->cv.wait(lock, [gate] { return gate->release_first; });
        }
        lock.unlock();

        bytes->assign(
            request.max_bytes,
            static_cast<std::byte>(static_cast<unsigned char>(request.record_index & 0xffU)));
        error->clear();
        return true;
    };
}

void wait_first_entered(const std::shared_ptr<ExecutorGate>& gate) {
    std::unique_lock<std::mutex> lock(gate->mutex);
    require(
        gate->cv.wait_for(lock, 5s, [gate] { return gate->first_entered; }),
        "prefetch executor did not enter first call");
}

void release_first(const std::shared_ptr<ExecutorGate>& gate) {
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->release_first = true;
    }
    gate->cv.notify_all();
}

void test_lazy_thread_start() {
    SourceWindowPrefetchWorker worker({}, [](
                                               const std::filesystem::path&,
                                               const SourceWindowPrefetchRequest& request,
                                               std::vector<std::byte>* bytes,
                                               std::string* error) {
        bytes->assign(request.max_bytes, std::byte{0x2a});
        error->clear();
        return true;
    });
    const PrefetchTicket ticket{10U, 1};

    SourceWindowPrefetchStatus status = worker.status();
    require(!status.thread_started, "constructor eagerly started prefetch thread");
    require(status.thread_starts == 0U, "constructor reported prefetch thread start");

    worker.set_authority_ticket(ticket);
    status = worker.status();
    require(!status.thread_started, "authority update eagerly started prefetch thread");

    require(
        worker.request(make_request(0U, 0U, 0U, ticket)) ==
            SourceWindowPrefetchScheduleResult::invalid,
        "lazy-start invalid request unexpectedly admitted");
    require(!worker.status().thread_started, "invalid request started prefetch thread");

    require(
        worker.request(make_request(0U, 0U, 1U, PrefetchTicket{10U, 0})) ==
            SourceWindowPrefetchScheduleResult::stale,
        "lazy-start stale request unexpectedly admitted");
    require(!worker.status().thread_started, "stale request started prefetch thread");

    require(
        worker.request(make_request(0U, 0U, 4U, ticket)) ==
            SourceWindowPrefetchScheduleResult::accepted,
        "first valid lazy-start request rejected");
    status = worker.status();
    require(status.thread_started, "first valid request did not start prefetch thread");
    require(status.thread_starts == 1U, "first valid request started unexpected thread count");
    require(worker.wait_idle_for(5s), "lazy-start request did not become idle");

    SourceWindowPrefetchResult result;
    require(worker.try_take_ready(&result), "lazy-start result missing");
    require(result.succeeded, "lazy-start result failed");

    require(
        worker.request(make_request(1U, 0U, 4U, ticket)) ==
            SourceWindowPrefetchScheduleResult::accepted,
        "second valid lazy-start request rejected");
    require(worker.wait_idle_for(5s), "second lazy-start request did not become idle");
    require(worker.status().thread_starts == 1U,
            "existing worker thread was recreated for later request");
}

void test_real_store_prefetch() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "zevryon-source-window-prefetch-tests";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    const std::vector<std::byte> payload = bytes_of("abcdefghijklmnopqrstuvwxyz");
    {
        StoreWriter writer(root);
        std::string error;
        require(
            writer.append(7U, std::span<const std::byte>(payload), &error),
            "real-store append failed");
        CorpusMetadata metadata;
        metadata.logical_utf8_bytes = payload.size();
        metadata.logical_records = 1U;
        metadata.logical_nodes = 1U;
        StoreStats stats;
        require(writer.finalize(metadata, &stats, &error), "real-store finalize failed");
    }

    {
        SourceWindowPrefetchWorker worker(root);
        require(!worker.status().thread_started,
                "real-store worker started before first accepted request");
        const PrefetchTicket ticket{11U, 1};
        worker.set_authority_ticket(ticket);
        require(
            worker.request(make_request(0U, 3U, 5U, ticket)) ==
                SourceWindowPrefetchScheduleResult::accepted,
            "real-store prefetch request rejected");
        require(worker.wait_idle_for(5s), "real-store prefetch did not become idle");

        SourceWindowPrefetchResult result;
        require(worker.try_take_ready(&result), "real-store prefetch result missing");
        require(result.succeeded, "real-store prefetch result failed");
        require(result.error.empty(), "real-store prefetch returned diagnostic");
        require(text_of(result.bytes) == "defgh", "real-store prefetch bytes mismatch");

        const SourceWindowPrefetchStatus status = worker.status();
        require(status.thread_started, "real-store accepted work did not start worker thread");
        require(status.thread_starts == 1U, "real-store worker thread start count mismatch");
        require(status.runs_started == 1U, "real-store run count mismatch");
        require(status.runs_succeeded == 1U, "real-store success count mismatch");
        require(status.runs_failed == 0U, "real-store failure count mismatch");
    }

    std::filesystem::remove_all(root, ignored);
}

void test_bounded_latest_pending_and_coalescing() {
    const auto gate = std::make_shared<ExecutorGate>();
    SourceWindowPrefetchWorker worker({}, gated_executor(gate));
    const PrefetchTicket ticket{20U, 1};
    worker.set_authority_ticket(ticket);

    const SourceWindowPrefetchRequest first = make_request(1U, 0U, 4U, ticket);
    const SourceWindowPrefetchRequest second = make_request(2U, 0U, 4U, ticket);
    const SourceWindowPrefetchRequest latest = make_request(3U, 0U, 4U, ticket);

    require(
        worker.request(first) == SourceWindowPrefetchScheduleResult::accepted,
        "first bounded request rejected");
    wait_first_entered(gate);
    require(
        worker.request(second) == SourceWindowPrefetchScheduleResult::accepted,
        "second bounded request rejected");
    require(
        worker.request(second) == SourceWindowPrefetchScheduleResult::coalesced,
        "duplicate pending request did not coalesce");
    require(
        worker.request(latest) == SourceWindowPrefetchScheduleResult::replaced,
        "latest request did not replace pending work");

    release_first(gate);
    require(worker.wait_idle_for(5s), "bounded latest-pending worker did not become idle");

    SourceWindowPrefetchResult result;
    require(worker.try_take_ready(&result), "latest pending result missing");
    require(result.succeeded, "latest pending result failed");
    require(result.request == latest, "worker did not publish latest pending request");
    require(result.bytes.size() == latest.max_bytes, "latest result size mismatch");
    require(
        std::to_integer<unsigned char>(result.bytes.front()) == 3U,
        "latest result payload mismatch");

    const SourceWindowPrefetchStatus status = worker.status();
    require(status.thread_starts == 1U, "bounded worker created more than one thread");
    require(status.requests_accepted == 2U, "accepted request count mismatch");
    require(status.requests_coalesced == 1U, "coalesced request count mismatch");
    require(status.requests_replaced == 1U, "replaced request count mismatch");
    require(status.runs_started == 2U, "bounded worker ran unexpected number of jobs");
    require(status.ready_replacements == 1U, "ready result was not bounded/replaced");
}

void test_epoch_change_drops_running_result() {
    const auto gate = std::make_shared<ExecutorGate>();
    SourceWindowPrefetchWorker worker({}, gated_executor(gate));
    const PrefetchTicket forward{30U, 1};
    const PrefetchTicket reverse{31U, -1};
    worker.set_authority_ticket(forward);

    const SourceWindowPrefetchRequest old_request = make_request(4U, 0U, 8U, forward);
    require(
        worker.request(old_request) == SourceWindowPrefetchScheduleResult::accepted,
        "old epoch request rejected");
    wait_first_entered(gate);

    worker.set_authority_ticket(reverse);
    release_first(gate);
    require(worker.wait_idle_for(5s), "stale-running worker did not become idle");

    SourceWindowPrefetchResult result;
    require(!worker.try_take_ready(&result), "stale running result was published");
    require(
        worker.request(old_request) == SourceWindowPrefetchScheduleResult::stale,
        "old epoch request was not rejected after reversal");

    const SourceWindowPrefetchRequest current = make_request(5U, 1U, 6U, reverse);
    require(
        worker.request(current) == SourceWindowPrefetchScheduleResult::accepted,
        "current epoch request rejected");
    require(worker.wait_idle_for(5s), "current epoch request did not become idle");
    require(worker.try_take_ready(&result), "current epoch result missing");
    require(result.request == current, "current epoch result identity mismatch");

    const SourceWindowPrefetchStatus status = worker.status();
    require(status.thread_starts == 1U, "epoch change recreated worker thread");
    require(status.stale_results_dropped == 1U, "stale running result drop not counted");
    require(status.requests_stale == 1U, "stale request rejection not counted");
}

void test_authority_change_invalidates_pending_and_ready() {
    const auto gate = std::make_shared<ExecutorGate>();
    SourceWindowPrefetchWorker worker({}, gated_executor(gate));
    const PrefetchTicket first_ticket{40U, 1};
    const PrefetchTicket second_ticket{41U, -1};
    worker.set_authority_ticket(first_ticket);

    require(
        worker.request(make_request(6U, 0U, 4U, first_ticket)) ==
            SourceWindowPrefetchScheduleResult::accepted,
        "authority test first request rejected");
    wait_first_entered(gate);
    require(
        worker.request(make_request(7U, 0U, 4U, first_ticket)) ==
            SourceWindowPrefetchScheduleResult::accepted,
        "authority test pending request rejected");
    worker.set_authority_ticket(second_ticket);
    release_first(gate);
    require(worker.wait_idle_for(5s), "authority test worker did not become idle");

    const SourceWindowPrefetchStatus status = worker.status();
    require(status.thread_starts == 1U, "authority change recreated worker thread");
    require(status.pending_cancellations == 1U, "stale pending request not cancelled");
    require(status.stale_results_dropped == 1U, "stale running result not dropped");
    require(status.runs_started == 1U, "cancelled pending request still executed");
}

void test_ready_invalidation() {
    SourceWindowPrefetchExecutor executor = [](
                                                const std::filesystem::path&,
                                                const SourceWindowPrefetchRequest& request,
                                                std::vector<std::byte>* bytes,
                                                std::string* error) {
        bytes->assign(request.max_bytes, std::byte{0x5a});
        error->clear();
        return true;
    };
    SourceWindowPrefetchWorker worker({}, std::move(executor));
    const PrefetchTicket first{50U, 1};
    worker.set_authority_ticket(first);
    require(
        worker.request(make_request(8U, 0U, 4U, first)) ==
            SourceWindowPrefetchScheduleResult::accepted,
        "ready invalidation request rejected");
    require(worker.wait_idle_for(5s), "ready invalidation worker did not become idle");
    require(worker.status().ready, "ready result not published");

    worker.set_authority_ticket(PrefetchTicket{51U, -1});
    SourceWindowPrefetchResult result;
    require(!worker.try_take_ready(&result), "stale ready result survived authority change");
    require(worker.status().ready_invalidations == 1U, "ready invalidation not counted");
}

void test_invalid_and_stopped_requests() {
    SourceWindowPrefetchWorker worker({}, [](
                                               const std::filesystem::path&,
                                               const SourceWindowPrefetchRequest&,
                                               std::vector<std::byte>*,
                                               std::string*) { return true; });
    const PrefetchTicket ticket{60U, 1};
    worker.set_authority_ticket(ticket);
    require(
        worker.request(make_request(0U, 0U, 0U, ticket)) ==
            SourceWindowPrefetchScheduleResult::invalid,
        "zero-byte request admitted");
    require(
        worker.request(make_request(0U, 0U, kIoWindowBytes + 1U, ticket)) ==
            SourceWindowPrefetchScheduleResult::invalid,
        "oversized request admitted");
    require(
        worker.request(make_request(0U, 0U, 1U, PrefetchTicket{60U, 0})) ==
            SourceWindowPrefetchScheduleResult::stale,
        "stationary request admitted");
    require(!worker.status().thread_started,
            "invalid/stale-only worker created an idle background thread");

    worker.stop();
    require(
        worker.request(make_request(0U, 0U, 1U, ticket)) ==
            SourceWindowPrefetchScheduleResult::stopped,
        "request admitted after stop");
    require(!worker.status().thread_started,
            "never-used worker started a thread during stop");
}

} // namespace

int main() {
    test_lazy_thread_start();
    test_real_store_prefetch();
    test_bounded_latest_pending_and_coalescing();
    test_epoch_change_drops_running_result();
    test_authority_change_invalidates_pending_and_ready();
    test_ready_invalidation();
    test_invalid_and_stopped_requests();
    std::cout << "Zevryon source-window prefetch worker tests passed\n";
    return 0;
}
