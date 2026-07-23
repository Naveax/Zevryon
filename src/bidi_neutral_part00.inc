#include "bidi_neutral.hpp"

#include "unicode_bidi_brackets.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace zevryon::text {
namespace {

constexpr std::size_t kMaximumBracketStack = 63U;
constexpr std::uint8_t kContainsLeft = 1U << 0U;
constexpr std::uint8_t kContainsRight = 1U << 1U;

class SequenceCursor {
public:
    SequenceCursor(
        const BidiSequenceTopology& topology,
        const BidiIsolatingRunSequence& sequence) noexcept
        : topology_(&topology), sequence_(&sequence) {}

    bool valid() const noexcept {
        return run_offset_ < sequence_->run_count;
    }

    std::uint32_t active_index() const noexcept {
        const std::size_t link =
            static_cast<std::size_t>(sequence_->first_run_link) + run_offset_;
        const BidiLevelRun& run =
            topology_->level_runs[topology_->sequence_run_indices[link]];
        return static_cast<std::uint32_t>(
            static_cast<std::size_t>(run.first_active_index) + unit_offset_);
    }

    void advance() noexcept {
        if (!valid()) {
            return;
        }
        const std::size_t link =
            static_cast<std::size_t>(sequence_->first_run_link) + run_offset_;
        const BidiLevelRun& run =
            topology_->level_runs[topology_->sequence_run_indices[link]];
        ++unit_offset_;
        if (unit_offset_ >= run.active_count) {
            unit_offset_ = 0U;
            ++run_offset_;
        }
    }

    bool operator==(const SequenceCursor& other) const noexcept {
        return topology_ == other.topology_ &&
               sequence_ == other.sequence_ &&
               run_offset_ == other.run_offset_ &&
               unit_offset_ == other.unit_offset_;
    }

private:
    const BidiSequenceTopology* topology_{nullptr};
    const BidiIsolatingRunSequence* sequence_{nullptr};
    std::size_t run_offset_{0};
    std::size_t unit_offset_{0};
};

struct BracketStackEntry {
    std::uint32_t expected_closing{0};
    std::uint32_t open_active{0};
    std::uint32_t open_ordinal{0};
    BidiClass preceding_strong{BidiClass::L};
    std::uint8_t strong_flags{0};
};

struct BracketPair {
    std::uint32_t open_active{0};
    std::uint32_t close_active{0};
    std::uint32_t open_ordinal{0};
    BidiClass preceding_strong{BidiClass::L};
    std::uint8_t strong_flags{0};
    std::uint16_t reserved{0};
};

static_assert(sizeof(BracketPair) <= 16U);

void clear_error(BidiNeutralError* error) noexcept {
    if (error != nullptr) {
        error->kind = BidiNeutralErrorKind::None;
        error->active_index = 0U;
        error->message.clear();
    }
}

bool fail(
    BidiNeutralErrorKind kind,
    std::size_t active_index,
    const char* message,
    BidiNeutralError* error) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->active_index = active_index;
        try {
            error->message = message;
        } catch (...) {
            error->message.clear();
        }
