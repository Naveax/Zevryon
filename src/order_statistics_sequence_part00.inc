#include "order_statistics_sequence.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace zevryon::massivedoc {
namespace {

bool checked_add(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t* output) noexcept {
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
        return false;
    }
    *output = lhs + rhs;
    return true;
}



} // namespace

struct ChunkedOrderStatisticsSequence::Impl {
    struct Chunk {
        std::vector<std::uint64_t> logical_ids;
        std::vector<std::uint64_t> text_bytes;
        std::vector<std::uint32_t> heights_q8;
        std::vector<std::uint64_t> search_summaries;

        std::size_t size() const noexcept { return logical_ids.size(); }

        SequenceRecord at(std::size_t index) const noexcept {
            return SequenceRecord{logical_ids[index], text_bytes[index], heights_q8[index], search_summaries[index]};
        }

        void reserve(std::size_t count) {
            logical_ids.reserve(count);
            text_bytes.reserve(count);
            heights_q8.reserve(count);
            search_summaries.reserve(count);
        }

        void insert(std::size_t index, const SequenceRecord& record) {
            logical_ids.insert(logical_ids.begin() + static_cast<std::ptrdiff_t>(index), record.logical_id);
            text_bytes.insert(text_bytes.begin() + static_cast<std::ptrdiff_t>(index), record.text_bytes);
            heights_q8.insert(heights_q8.begin() + static_cast<std::ptrdiff_t>(index), record.height_q8);
            search_summaries.insert(search_summaries.begin() + static_cast<std::ptrdiff_t>(index), record.search_summary);
        }

        SequenceRecord erase(std::size_t index) {
            const SequenceRecord record = at(index);
            logical_ids.erase(logical_ids.begin() + static_cast<std::ptrdiff_t>(index));
            text_bytes.erase(text_bytes.begin() + static_cast<std::ptrdiff_t>(index));
            heights_q8.erase(heights_q8.begin() + static_cast<std::ptrdiff_t>(index));
            search_summaries.erase(search_summaries.begin() + static_cast<std::ptrdiff_t>(index));
            return record;
        }
    };

    struct Node {
        explicit Node(std::uint32_t reserve_count) {
            chunk.reserve(reserve_count);
        }

        Chunk chunk;
        std::unique_ptr<Node> left;
        std::unique_ptr<Node> right;
        SequenceAggregate aggregate{};
        std::uint64_t chunk_count{1};
        std::uint32_t tree_height{1U};
    };

    explicit Impl(std::uint32_t max_chunk) : max_records_per_chunk(max_chunk) {}

    std::unique_ptr<Node> root;
    std::uint32_t max_records_per_chunk{256U};

    static const SequenceAggregate& aggregate_of(const std::unique_ptr<Node>& node) noexcept {
        static const SequenceAggregate empty{};
        return node ? node->aggregate : empty;
    }

    static std::uint64_t chunks_of(const std::unique_ptr<Node>& node) noexcept {
        return node ? node->chunk_count : 0U;
    }

    static std::uint32_t height_of(const std::unique_ptr<Node>& node) noexcept {
        return node ? node->tree_height : 0U;
    }

    static bool compute_chunk_aggregate(const Chunk& chunk, SequenceAggregate* output) noexcept {
        SequenceAggregate aggregate;
        aggregate.record_count = static_cast<std::uint64_t>(chunk.size());
        for (std::size_t index = 0U; index < chunk.size(); ++index) {
            if (!checked_add(aggregate.text_bytes, chunk.text_bytes[index], &aggregate.text_bytes) ||
                !checked_add(aggregate.layout_height_q8, chunk.heights_q8[index], &aggregate.layout_height_q8)) {
                return false;
            }
            aggregate.search_summary |= chunk.search_summaries[index];
        }
        *output = aggregate;
        return true;
    }

    static bool combine(
        const SequenceAggregate& left,
        const SequenceAggregate& middle,
        const SequenceAggregate& right,
        SequenceAggregate* output) noexcept {
        SequenceAggregate aggregate;
        if (!checked_add(left.record_count, middle.record_count, &aggregate.record_count) ||
            !checked_add(aggregate.record_count, right.record_count, &aggregate.record_count) ||
            !checked_add(left.text_bytes, middle.text_bytes, &aggregate.text_bytes) ||
            !checked_add(aggregate.text_bytes, right.text_bytes, &aggregate.text_bytes) ||
            !checked_add(left.layout_height_q8, middle.layout_height_q8, &aggregate.layout_height_q8) ||
            !checked_add(aggregate.layout_height_q8, right.layout_height_q8, &aggregate.layout_height_q8)) {
            return false;
        }
        aggregate.search_summary = left.search_summary | middle.search_summary | right.search_summary;
        *output = aggregate;
        return true;
    }

    static bool refresh(Node* node) noexcept {
        if (node == nullptr) {
            return true;
        }
        SequenceAggregate middle;
        if (!compute_chunk_aggregate(node->chunk, &middle) ||
            !combine(aggregate_of(node->left), middle, aggregate_of(node->right), &node->aggregate)) {
            return false;
        }
        const std::uint64_t left_chunks = chunks_of(node->left);
        const std::uint64_t right_chunks = chunks_of(node->right);
        if (left_chunks > std::numeric_limits<std::uint64_t>::max() - 1U ||
            left_chunks + 1U > std::numeric_limits<std::uint64_t>::max() - right_chunks) {
            return false;
        }
        node->chunk_count = left_chunks + 1U + right_chunks;
        node->tree_height = 1U + std::max(height_of(node->left), height_of(node->right));
        return true;
    }

    std::unique_ptr<Node> make_node(Chunk chunk) {
        auto node = std::make_unique<Node>(max_records_per_chunk);
        node->chunk = std::move(chunk);
        if (!refresh(node.get())) {
            return nullptr;
        }
        return node;
    }

    static std::unique_ptr<Node> rotate_left(std::unique_ptr<Node> root) noexcept {
        auto pivot = std::move(root->right);
        root->right = std::move(pivot->left);
        static_cast<void>(refresh(root.get()));
        pivot->left = std::move(root);
        static_cast<void>(refresh(pivot.get()));
        return pivot;
    }

    static std::unique_ptr<Node> rotate_right(std::unique_ptr<Node> root) noexcept {
        auto pivot = std::move(root->left);
        root->left = std::move(pivot->right);
        static_cast<void>(refresh(root.get()));
        pivot->right = std::move(root);
        static_cast<void>(refresh(pivot.get()));
        return pivot;
    }

    static std::unique_ptr<Node> rebalance(std::unique_ptr<Node> root) noexcept {
        if (!root) {
            return root;
        }
        static_cast<void>(refresh(root.get()));
        const std::int64_t balance = static_cast<std::int64_t>(height_of(root->left)) -
