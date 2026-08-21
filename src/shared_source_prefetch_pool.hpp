#pragma once

#include "hot_scroll_source_prefetch.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace zevryon::massivedoc {

class SharedRecordLengthAuthority;

struct SharedSourcePrefetchPoolConfig {
    std::size_t worker_count{2U};

    // There is no finite product-level tab/session ceiling. The SIZE_MAX
    // default makes registry admission policy-unbounded; actual admission can
    // still fail naturally if the process cannot allocate tiny session metadata.
    std::size_t max_sessions{std::numeric_limits<std::size_t>::max()};

    // Heavy speculative payload retention is bounded independently of tab count.
    std::size_t max_ready_bytes{2U * 1024U * 1024U};

    // Optional process-owned authority. Non-owning. When supplied, built-in
    // workers may resolve record length off the UI thread and canonicalize a
    // request before payload I/O.
    SharedRecordLengthAuthority* record_length_authority{nullptr};

    bool valid() const noexcept;
};

enum class SharedSourcePrefetchScheduleResult : std::uint8_t {
    accepted = 0U,
    coalesced,
    replaced,
    stale,
    inactive,
    unknown_session,
    invalid,
    stopped,
    thread_start_failed,
};

struct SharedSourcePrefetchPoolStatus {
    std::size_t configured_workers{0U};
    std::size_t live_threads{0U};
    std::size_t sessions{0U};
    std::size_t active_sessions{0U};
    std::size_t queued_sessions{0U};
    std::size_t running_sessions{0U};
    std::size_t ready_results{0U};
    std::size_t ready_bytes{0U};
    std::size_t ready_peak_bytes{0U};
    std::size_t maximum_queue_depth{0U};
    std::uint64_t thread_starts{0U};
    std::uint64_t thread_start_failures{0U};
    std::uint64_t requests_total{0U};
    std::uint64_t requests_accepted{0U};
    std::uint64_t requests_coalesced{0U};
    std::uint64_t requests_replaced{0U};
    std::uint64_t requests_stale{0U};
    std::uint64_t requests_inactive{0U};
    std::uint64_t requests_unknown_session{0U};
    std::uint64_t requests_invalid{0U};
    std::uint64_t requests_stopped{0U};
    std::uint64_t pending_cancellations{0U};
    std::uint64_t ready_invalidations{0U};
    std::uint64_t fairness_requeues{0U};
    std::uint64_t runs_started{0U};
    std::uint64_t runs_succeeded{0U};
    std::uint64_t runs_failed{0U};
    std::uint64_t runs_suppressed{0U};
    std::uint64_t stale_results_dropped{0U};
    std::uint64_t inactive_results_dropped{0U};
    std::uint64_t closed_results_dropped{0U};
    std::uint64_t ready_replacements{0U};
    std::uint64_t ready_budget_drops{0U};
    std::uint64_t canonicalized_results{0U};
    std::uint64_t worker_eof_suppressions{0U};
    std::uint64_t record_length_resolve_failures{0U};
    std::uint64_t record_length_learns{0U};
    bool stopped{false};
};

// Legacy contract. Kept so existing embedders do not need a flag-day migration.
using SharedSourcePrefetchExecutor = std::function<bool(
    const std::filesystem::path&,
    std::uint64_t,
    const SourceWindowPrefetchRequest&,
    std::vector<std::byte>*,
    std::string*)>;

// V2 explicitly returns the request identity that actually produced the bytes.
// Only offset/size may be narrowed; record identity and authority ticket remain
// immutable and are validated by the pool before publication.
struct SharedSourcePrefetchExecution {
    SourceWindowPrefetchRequest canonical_request{};
    std::vector<std::byte> bytes;
    bool suppressed{false};
    bool record_length_resolve_failed{false};
    bool record_length_learned{false};
};

struct SharedSourcePrefetchExecutorV2 {
    using Function = std::function<bool(
        const std::filesystem::path&,
        std::uint64_t,
        const SourceWindowPrefetchRequest&,
        SharedSourcePrefetchExecution*,
        std::string*)>;

    Function run;

    explicit operator bool() const noexcept {
        return static_cast<bool>(run);
    }
};

class SharedSourcePrefetchPool final {
public:
    explicit SharedSourcePrefetchPool(
        SharedSourcePrefetchPoolConfig config = {});
    SharedSourcePrefetchPool(
        SharedSourcePrefetchPoolConfig config,
        SharedSourcePrefetchExecutor executor);
    SharedSourcePrefetchPool(
        SharedSourcePrefetchPoolConfig config,
        SharedSourcePrefetchExecutorV2 executor);
    ~SharedSourcePrefetchPool();

    SharedSourcePrefetchPool(const SharedSourcePrefetchPool&) = delete;
    SharedSourcePrefetchPool& operator=(const SharedSourcePrefetchPool&) = delete;
    SharedSourcePrefetchPool(SharedSourcePrefetchPool&&) = delete;
    SharedSourcePrefetchPool& operator=(SharedSourcePrefetchPool&&) = delete;

    bool valid() const noexcept;
    bool open_session(
        std::uint64_t session_id,
        std::filesystem::path store_root,
        PrefetchTicket authority_ticket,
        bool active = true);
    bool close_session(std::uint64_t session_id);
    bool set_session_authority(
        std::uint64_t session_id,
        PrefetchTicket authority_ticket,
        bool active);

    SharedSourcePrefetchScheduleResult request(
        std::uint64_t session_id,
        SourceWindowPrefetchRequest request);
    bool try_take_ready(
        std::uint64_t session_id,
        SourceWindowPrefetchResult* result);
    bool wait_idle_for(std::chrono::milliseconds timeout);
    SharedSourcePrefetchPoolStatus status() const;
    void stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace zevryon::massivedoc
