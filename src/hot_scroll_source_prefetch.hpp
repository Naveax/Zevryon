#pragma once

#include "frame_budget_scheduler.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace zevryon::massivedoc {

enum class SourceWindowPrefetchScheduleResult : std::uint8_t {
    accepted = 0U,
    coalesced,
    replaced,
    stale,
    invalid,
    stopped,
};

struct SourceWindowPrefetchRequest {
    std::uint64_t record_index{0U};
    std::uint64_t byte_offset{0U};
    std::size_t max_bytes{0U};
    PrefetchTicket ticket{};

    // Immutable source edge that produced the speculative prediction. This is
    // a scheduling hint, not part of source identity. It lets a worker clamp a
    // velocity lead against record EOF without doing any UI-thread metadata I/O.
    std::uint64_t visible_edge_offset{0U};
    bool has_visible_edge_offset{false};

    bool operator==(const SourceWindowPrefetchRequest&) const noexcept = default;
};

struct SourceWindowPrefetchResult {
    SourceWindowPrefetchRequest request{};
    std::vector<std::byte> bytes;
    bool succeeded{false};
    std::string error;
};

struct SourceWindowPrefetchStatus {
    PrefetchTicket authority_ticket{};
    bool thread_started{false};
    bool running{false};
    bool pending{false};
    bool ready{false};
    bool stopped{false};
    std::uint64_t thread_starts{0U};
    std::uint64_t requests_total{0U};
    std::uint64_t requests_accepted{0U};
    std::uint64_t requests_coalesced{0U};
    std::uint64_t requests_replaced{0U};
    std::uint64_t requests_stale{0U};
    std::uint64_t requests_invalid{0U};
    std::uint64_t requests_stopped{0U};
    std::uint64_t pending_cancellations{0U};
    std::uint64_t ready_invalidations{0U};
    std::uint64_t runs_started{0U};
    std::uint64_t runs_succeeded{0U};
    std::uint64_t runs_failed{0U};
    std::uint64_t stale_results_dropped{0U};
    std::uint64_t ready_replacements{0U};
};

using SourceWindowPrefetchExecutor = std::function<bool(
    const std::filesystem::path&,
    const SourceWindowPrefetchRequest&,
    std::vector<std::byte>*,
    std::string*)>;

class SourceWindowPrefetchWorker final {
public:
    explicit SourceWindowPrefetchWorker(std::filesystem::path store_root);
    SourceWindowPrefetchWorker(
        std::filesystem::path store_root,
        SourceWindowPrefetchExecutor executor);
    ~SourceWindowPrefetchWorker();

    SourceWindowPrefetchWorker(const SourceWindowPrefetchWorker&) = delete;
    SourceWindowPrefetchWorker& operator=(const SourceWindowPrefetchWorker&) = delete;
    SourceWindowPrefetchWorker(SourceWindowPrefetchWorker&&) = delete;
    SourceWindowPrefetchWorker& operator=(SourceWindowPrefetchWorker&&) = delete;

    void set_authority_ticket(PrefetchTicket ticket);
    PrefetchTicket authority_ticket() const;
    SourceWindowPrefetchScheduleResult request(SourceWindowPrefetchRequest request);
    bool try_take_ready(SourceWindowPrefetchResult* result);
    bool wait_idle_for(std::chrono::milliseconds timeout);
    SourceWindowPrefetchStatus status() const;
    void stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace zevryon::massivedoc
