#include "shared_source_prefetch_pool.hpp"

#include "massivedoc_store.hpp"
#include "prefetch_record_bounds.hpp"
#include "shared_record_length_authority.hpp"
#include "store_record_length_probe.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

namespace zevryon::massivedoc {
namespace {

std::uint64_t saturating_increment(std::uint64_t value) noexcept {
    return value == UINT64_MAX ? value : value + 1U;
}

bool canonical_execution_valid(
    const SourceWindowPrefetchRequest& original,
    const SharedSourcePrefetchExecution& execution,
    bool succeeded) noexcept {
    const SourceWindowPrefetchRequest& canonical = execution.canonical_request;
    if (canonical.record_index != original.record_index ||
        canonical.ticket != original.ticket) {
        return false;
    }
    if (execution.suppressed) {
        return succeeded && execution.bytes.empty();
    }
    if (canonical.max_bytes == 0U ||
        canonical.max_bytes > original.max_bytes ||
        canonical.max_bytes > kIoWindowBytes) {
        return false;
    }
    return !succeeded || execution.bytes.size() <= canonical.max_bytes;
}

} // namespace

bool SharedSourcePrefetchPoolConfig::valid() const noexcept {
    return worker_count > 0U && worker_count <= 64U && max_ready_bytes > 0U;
}

struct SharedSourcePrefetchPool::State {
    struct SessionState {
        SessionState(
            std::uint64_t id_value,
            std::filesystem::path root_value,
            PrefetchTicket authority_value,
            bool active_value)
            : id(id_value),
              root(std::move(root_value)),
              authority(authority_value),
              active(active_value) {}

        std::uint64_t id{0U};
        std::filesystem::path root;
        PrefetchTicket authority{};
        bool active{true};
        bool closed{false};
        bool queued{false};
        bool running{false};
        bool counted{true};
        std::uint64_t queue_epoch{0U};
        std::optional<SourceWindowPrefetchRequest> pending;
        std::optional<SourceWindowPrefetchResult> ready;
        std::size_t ready_charge_bytes{0U};
    };

    struct QueueEntry {
        std::shared_ptr<SessionState> session;
        std::uint64_t queue_epoch{0U};
    };

    State(
        SharedSourcePrefetchPoolConfig config_value,
        SharedSourcePrefetchExecutor legacy_executor_value,
        SharedSourcePrefetchExecutorV2 executor_v2_value)
        : config(config_value),
          legacy_executor(std::move(legacy_executor_value)),
          executor_v2(std::move(executor_v2_value)) {}

    bool execute_default(
        const std::shared_ptr<SessionState>& session,
        const SourceWindowPrefetchRequest& request_value,
        SharedSourcePrefetchExecution* execution,
        std::string* error) {
        execution->canonical_request = request_value;

        if (config.record_length_authority != nullptr &&
            request_value.has_visible_edge_offset) {
            std::uint64_t record_length = 0U;
            std::string metadata_error;
            const bool resolved = config.record_length_authority->query(
                session->root,
                request_value.record_index,
                [](const std::filesystem::path& root,
                   std::uint64_t record_index,
                   std::uint64_t* length,
                   std::string* resolver_error) {
                    return probe_store_record_length(
                        root,
                        record_index,
                        length,
                        resolver_error);
                },
                &record_length,
                &metadata_error);
            if (resolved) {
                const RecordBoundPrefetchDecision decision =
                    clamp_prefetch_to_record(
                        request_value.ticket.direction,
                        request_value.visible_edge_offset,
                        request_value.byte_offset,
                        request_value.max_bytes,
                        record_length);
                if (!decision.should_issue) {
                    execution->canonical_request.byte_offset =
                        decision.byte_offset;
                    execution->canonical_request.max_bytes = 0U;
                    execution->suppressed = decision.eof_suppressed;
                    error->clear();
                    return decision.eof_suppressed;
                }
                execution->canonical_request.byte_offset =
                    decision.byte_offset;
                execution->canonical_request.max_bytes =
                    decision.request_bytes;
            } else {
                execution->record_length_resolve_failed = true;
            }
        }

        StoreReader store(session->root);
        if (!store.open(error)) {
            return false;
        }
        const SourceWindowPrefetchRequest& canonical =
            execution->canonical_request;
        if (!store.read_record_slice(
                canonical.record_index,
                canonical.byte_offset,
                canonical.max_bytes,
                &execution->bytes,
                error)) {
            return false;
        }

        if (config.record_length_authority != nullptr &&
            execution->bytes.size() < canonical.max_bytes &&
            canonical.byte_offset <= UINT64_MAX - execution->bytes.size()) {
            const std::uint64_t learned_length =
                canonical.byte_offset +
                static_cast<std::uint64_t>(execution->bytes.size());
            std::string learn_error;
            if (config.record_length_authority->remember(
                    session->root,
                    canonical.record_index,
                    learned_length,
                    &learn_error)) {
                execution->record_length_learned = true;
            }
        }
        error->clear();
        return true;
    }

    bool execute(
        const std::shared_ptr<SessionState>& session,
        const SourceWindowPrefetchRequest& request_value,
        SharedSourcePrefetchExecution* execution,
        std::string* error) {
        if (execution == nullptr || error == nullptr) {
            return false;
        }
        *execution = {};
        execution->canonical_request = request_value;

        if (executor_v2) {
            return executor_v2.run(
                session->root,
                session->id,
                request_value,
                execution,
                error);
        }
        if (legacy_executor) {
            const bool succeeded = legacy_executor(
                session->root,
                session->id,
                request_value,
                &execution->bytes,
                error);
