#include "massivedoc_generation_background.hpp"
#include "massivedoc_generation_sync.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

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
    std::mt19937_64 random(0x4d334241434b4752ULL);
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("zevryon-") + std::string(name) + "-" +
                       std::to_string(random()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "cannot clear background compaction fixture root");
    std::filesystem::create_directories(root / "segments", error);
    require(!error, "cannot create background compaction fixture segments");
    return root;
}

void cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "background compaction fixture cleanup failed");
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(stream), "cannot create background compaction fixture file");
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(stream), "cannot write background compaction fixture file");
}

struct Fixture {
    std::filesystem::path root;
    std::vector<std::byte> authority;
    std::vector<zevryon::massivedoc::GenerationSegmentInventory> segments;
};

Fixture make_fixture(std::string_view name) {
    Fixture fixture;
    fixture.root = temp_root(name);
    write_text(fixture.root / "records.idx", "records");
    write_text(fixture.root / "chunks.idx", "chunks");
    write_text(fixture.root / "search.bgm", "search");
    write_text(fixture.root / "segments" / "segment-00000000.bin", "payload");
    fixture.authority.assign(160U, std::byte{0});
    fixture.segments.push_back({0U, 7U});
    return fixture;
}

std::array<std::uint8_t, 32> identity_for(std::uint64_t generation) {
    std::array<std::uint8_t, 32> identity{};
    for (std::size_t index = 0U; index < identity.size(); ++index) {
        identity[index] = static_cast<std::uint8_t>(
            (generation + static_cast<std::uint64_t>(index)) & 0xffU);
    }
    return identity;
}

void publish_generations(Fixture* fixture, std::uint64_t count) {
    require(fixture != nullptr, "invalid background compaction fixture");
    std::string error;
    for (std::uint64_t generation = 1U; generation <= count; ++generation) {
        fixture->authority[0] = static_cast<std::byte>(
            static_cast<unsigned char>(generation & 0xffU));
        require(
            zevryon::massivedoc::publish_store_generation(
                fixture->root,
                generation,
                fixture->authority,
                identity_for(generation),
                fixture->segments,
                zevryon::massivedoc::GenerationPublicationCut::none,
                &error),
            error);
    }
}

struct BlockingExecutorGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::uint64_t calls{0U};
    bool released{false};
};

bool wait_for_calls(BlockingExecutorGate* gate, std::uint64_t target) {
    std::unique_lock<std::mutex> lock(gate->mutex);
    return gate->cv.wait_for(lock, 3s, [gate, target] {
        return gate->calls >= target;
    });
}

void release_gate(BlockingExecutorGate* gate) {
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->released = true;
    }
    gate->cv.notify_all();
}

zevryon::massivedoc::GenerationCompactionExecutor blocking_executor(
    BlockingExecutorGate* gate) {
    return [gate](
               const std::filesystem::path&,
               zevryon::massivedoc::GenerationCompactionConfig,
               zevryon::massivedoc::GenerationCompactionResult* result,
               std::string* error) {
        std::unique_lock<std::mutex> lock(gate->mutex);
        ++gate->calls;
        const std::uint64_t call = gate->calls;
        gate->cv.notify_all();
        gate->cv.wait(lock, [gate] { return gate->released; });
        result->authority_generation = call;
        error->clear();
        return true;
    };
}

void test_single_flight_coalesces_bounded_pending_work() {
    BlockingExecutorGate gate;
    zevryon::massivedoc::GenerationCompactionWorker worker(
        "synthetic-store",
        {},
        blocking_executor(&gate));

    require(
        worker.request() ==
            zevryon::massivedoc::GenerationCompactionScheduleResult::accepted,
        "first background compaction request was not accepted");
    require(wait_for_calls(&gate, 1U), "background compaction executor did not start");
    require(
        worker.request() ==
            zevryon::massivedoc::GenerationCompactionScheduleResult::accepted,
        "single pending background compaction was not accepted");
    for (std::uint32_t index = 0U; index < 128U; ++index) {
        require(
            worker.request() ==
                zevryon::massivedoc::GenerationCompactionScheduleResult::coalesced,
            "background compaction queue grew beyond one pending request");
    }

    const auto blocked = worker.status();
    require(blocked.running, "background compaction did not report in-flight work");
    require(blocked.pending, "background compaction lost its one pending request");
    require(blocked.requests_accepted == 2U, "background compaction accepted count mismatch");
    require(blocked.requests_coalesced == 128U, "background compaction coalesced count mismatch");

    release_gate(&gate);
    require(worker.wait_idle_for(3s), "background compaction did not become idle");
    const auto finished = worker.status();
    require(finished.runs_started == 2U, "coalesced background compaction executed wrong run count");
    require(finished.runs_succeeded == 2U, "background compaction success count mismatch");
    require(finished.runs_failed == 0U, "background compaction unexpectedly failed");

    worker.stop();
    require(
        worker.request() ==
            zevryon::massivedoc::GenerationCompactionScheduleResult::stopped,
        "stopped background compaction worker accepted new work");
    require(worker.status().requests_stopped == 1U, "stopped request accounting mismatch");
}

void test_pending_cancellation_does_not_interrupt_inflight_transaction() {
    BlockingExecutorGate gate;
    zevryon::massivedoc::GenerationCompactionWorker worker(
        "synthetic-cancel-store",
        {},
        blocking_executor(&gate));

    require(
        worker.request() ==
            zevryon::massivedoc::GenerationCompactionScheduleResult::accepted,
        "cancel test first request was not accepted");
    require(wait_for_calls(&gate, 1U), "cancel test executor did not start");
    require(
        worker.request() ==
            zevryon::massivedoc::GenerationCompactionScheduleResult::accepted,
        "cancel test pending request was not accepted");
    require(worker.cancel_pending(), "pending background compaction was not cancelled");
    require(!worker.cancel_pending(), "empty background compaction queue cancelled twice");

    release_gate(&gate);
    require(worker.wait_idle_for(3s), "cancelled background compaction did not become idle");
    const auto status = worker.status();
    require(status.runs_started == 1U, "pending cancellation interrupted or duplicated in-flight work");
    require(status.pending_cancellations == 1U, "pending cancellation accounting mismatch");
    worker.stop();
}

void test_executor_failure_is_contained_and_reported() {
    zevryon::massivedoc::GenerationCompactionWorker worker(
        "synthetic-failure-store",
        {},
        [](const std::filesystem::path&,
           zevryon::massivedoc::GenerationCompactionConfig,
           zevryon::massivedoc::GenerationCompactionResult*,
           std::string* error) {
            *error = "synthetic background compaction failure";
            return false;
        });
    require(
        worker.request() ==
            zevryon::massivedoc::GenerationCompactionScheduleResult::accepted,
        "failure test request was not accepted");
    require(worker.wait_idle_for(3s), "failure test worker did not become idle");
    const auto status = worker.status();
    require(status.runs_failed == 1U, "background executor failure was not counted");
    require(status.runs_succeeded == 0U, "failed background executor was counted successful");
    require(
        status.last_error == "synthetic background compaction failure",
        "background executor failure diagnostic mismatch");
    worker.stop();
}

void test_real_background_compaction_waits_for_transaction_boundary() {
    auto fixture = make_fixture("generation-background-real");
    publish_generations(&fixture, 4U);
    const auto journal = zevryon::massivedoc::store_generation_journal_path(fixture.root);
    const std::uint64_t before_bytes = std::filesystem::file_size(journal);

    zevryon::massivedoc::GenerationCompactionWorker worker(
        fixture.root,
        zevryon::massivedoc::GenerationCompactionConfig{2U});
    std::unique_lock<std::recursive_mutex> transaction_lock(
        zevryon::massivedoc::generation_transaction_mutex());
    require(
        worker.request() ==
            zevryon::massivedoc::GenerationCompactionScheduleResult::accepted,
        "real background compaction request was not accepted");
    require(
        !worker.wait_idle_for(20ms),
        "background compaction bypassed the generation transaction boundary");
    require(
        std::filesystem::file_size(journal) == before_bytes,
        "blocked background compaction mutated the live journal");

    transaction_lock.unlock();
    require(worker.wait_idle_for(5s), "real background compaction did not complete");
    const auto status = worker.status();
    require(status.runs_succeeded == 1U, status.last_error);
    require(status.last_result.authority_generation == 4U, "background compaction authority mismatch");
    require(status.last_result.retained_committed_generations == 2U, "background compaction retention mismatch");
    require(status.last_result.quarantined_stale_manifests == 2U, "background compaction stale quarantine mismatch");
    require(status.last_result.journal_bytes_after < before_bytes, "background compaction did not shrink journal");

    zevryon::massivedoc::GenerationRecovery recovery;
    std::string error;
    require(
        zevryon::massivedoc::recover_store_generation(
            fixture.root,
            &recovery,
            &error),
        error);
    require(recovery.found && recovery.generation == 4U, "background compaction changed authority");
    require(recovery.source_identity == identity_for(4U), "background compaction changed source identity");

    worker.stop();
    cleanup(fixture.root);
}

} // namespace

int main() {
    test_single_flight_coalesces_bounded_pending_work();
    test_pending_cancellation_does_not_interrupt_inflight_transaction();
    test_executor_failure_is_contained_and_reported();
    test_real_background_compaction_waits_for_transaction_boundary();
    std::cout << "Zevryon MassiveDoc background generation compaction tests passed\n";
    return 0;
}
