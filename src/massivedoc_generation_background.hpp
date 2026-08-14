#pragma once

#include "massivedoc_generation.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace zevryon::massivedoc {

enum class GenerationCompactionScheduleResult : std::uint32_t {
    accepted = 0U,
    coalesced = 1U,
    stopped = 2U,
};

struct GenerationBackgroundCompactionStatus {
    bool running{false};
    bool pending{false};
    bool stopped{false};
    std::uint64_t requests_total{0U};
    std::uint64_t requests_accepted{0U};
    std::uint64_t requests_coalesced{0U};
    std::uint64_t requests_stopped{0U};
    std::uint64_t pending_cancellations{0U};
    std::uint64_t runs_started{0U};
    std::uint64_t runs_succeeded{0U};
    std::uint64_t runs_failed{0U};
    GenerationCompactionResult last_result;
    std::string last_error;
};

using GenerationCompactionExecutor = std::function<bool(
    const std::filesystem::path&,
    GenerationCompactionConfig,
    GenerationCompactionResult*,
    std::string*)>;

class GenerationCompactionWorker final {
public:
    explicit GenerationCompactionWorker(
        std::filesystem::path store_root,
        GenerationCompactionConfig config = {});

    GenerationCompactionWorker(
        std::filesystem::path store_root,
        GenerationCompactionConfig config,
        GenerationCompactionExecutor executor);

    ~GenerationCompactionWorker();

    GenerationCompactionWorker(const GenerationCompactionWorker&) = delete;
    GenerationCompactionWorker& operator=(const GenerationCompactionWorker&) = delete;
    GenerationCompactionWorker(GenerationCompactionWorker&&) = delete;
    GenerationCompactionWorker& operator=(GenerationCompactionWorker&&) = delete;

    GenerationCompactionScheduleResult request();
    bool cancel_pending();
    bool wait_idle_for(std::chrono::milliseconds timeout);
    GenerationBackgroundCompactionStatus status() const;
    void stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace zevryon::massivedoc
