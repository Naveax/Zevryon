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
        std::vector<std::uint64_t> source_record_indices;

        std::size_t size() const noexcept { return logical_ids.size(); }

        SequenceRecord at(std::size_t index) const noexcept {
            return SequenceRecord{
                logical_ids[index],
                text_bytes[index],
                heights_q8[index],
                search_summaries[index],
                source_record_indices[index],
            };
        }

        void reserve(std::size_t count) {
            logical_ids.reserve(count);
            text_bytes.reserve(count);
            heights_q8.reserve(count);
            search_summaries.reserve(count);
            source_record_indices.reserve(count);
        }

        void insert(std::size_t index, const SequenceRecord& record) {
            const auto offset = static_cast<std::ptrdiff_t>(index);
            logical_ids.insert(logical_ids.begin() + offset, record.logical_id);
            text_bytes.insert(text_bytes.begin() + offset, record.text_bytes);
            heights_q8.insert(heights_q8.begin() + offset, record.height_q8);
            search_summaries.insert(search_summaries.begin() + offset, record.search_summary);
            source_record_indices.insert(source_record_indices.begin() + offset, record.source_record_index);
        }

        SequenceRecord erase(std::size_t index) {
            const SequenceRecord record = at(index);
            const auto offset = static_cast<std::ptrdiff_t>(index);
            logical_ids.erase(logical_ids.begin() + offset);
            text_bytes.erase(text_bytes.begin() + offset);
            heights_q8.erase(heights_q8.begin() + offset);
            search_summaries.erase(search_summaries.begin() + offset);
            source_record_indices.erase(source_record_indices.begin() + offset);
            return record;
        }
    };

    struct Node;
    using ChunkPtr = std::shared_ptr<const Chunk>;
    using NodePtr = std::shared_ptr<const Node>;

    struct Node {
        Node(
            ChunkPtr chunk_value,
            NodePtr left_value,
            NodePtr right_value,
            SequenceAggregate aggregate_value,
            std::uint64_t chunk_count_value,
            std::uint32_t tree_height_value)
            : chunk(std::move(chunk_value)),
              left(std::move(left_value)),
              right(std::move(right_value)),
              aggregate(aggregate_value),
              chunk_count(chunk_count_value),
              tree_height(tree_height_value) {}

        ChunkPtr chunk;
        NodePtr left;
        NodePtr right;
        SequenceAggregate aggregate{};
        std::uint64_t chunk_count{1U};
        std::uint32_t tree_height{1U};
    };

    Impl(std::uint32_t max_chunk, NodePtr root_value)
        : root(std::move(root_value)), max_records_per_chunk(max_chunk) {}

    NodePtr root;
    std::uint32_t max_records_per_chunk{256U};

    static const SequenceAggregate& aggregate_of(const NodePtr& node) noexcept {
        static const SequenceAggregate empty{};
        return node ? node->aggregate : empty;
    }

    static std::uint64_t chunks_of(const NodePtr& node) noexcept {
        return node ? node->chunk_count : 0U;
    }

    static std::uint32_t height_of(const NodePtr& node) noexcept {
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

    static bool make_node(
        ChunkPtr chunk,
        NodePtr left,
        NodePtr right,
        NodePtr* output,
        std::string* error) {
        SequenceAggregate middle;
        SequenceAggregate aggregate;
        if (!chunk || !compute_chunk_aggregate(*chunk, &middle) ||
            !combine(aggregate_of(left), middle, aggregate_of(right), &aggregate)) {
            *error = "sequence aggregate overflow";
            return false;
        }
        const std::uint64_t left_chunks = chunks_of(left);
        const std::uint64_t right_chunks = chunks_of(right);
        if (left_chunks > std::numeric_limits<std::uint64_t>::max() - 1U ||
            left_chunks + 1U > std::numeric_limits<std::uint64_t>::max() - right_chunks) {
            *error = "sequence chunk-count overflow";
            return false;
        }
        const std::uint32_t tree_height = 1U + std::max(height_of(left), height_of(right));
        *output = std::make_shared<Node>(
            std::move(chunk),
            std::move(left),
            std::move(right),
            aggregate,
            left_chunks + 1U + right_chunks,
            tree_height);
        return true;
    }

    static bool make_node(
        Chunk chunk,
        NodePtr left,
        NodePtr right,
        NodePtr* output,
        std::string* error) {
        return make_node(
            std::make_shared<const Chunk>(std::move(chunk)),
            std::move(left),
            std::move(right),
            output,
            error);
    }

    static bool rotate_left(const NodePtr& root, NodePtr* output, std::string* error) {
        const NodePtr pivot = root->right;
        NodePtr new_left;
        if (!make_node(root->chunk, root->left, pivot->left, &new_left, error)) {
            return false;
        }
        return make_node(pivot->chunk, std::move(new_left), pivot->right, output, error);
    }

    static bool rotate_right(const NodePtr& root, NodePtr* output, std::string* error) {
        const NodePtr pivot = root->left;
        NodePtr new_right;
        if (!make_node(root->chunk, pivot->right, root->right, &new_right, error)) {
            return false;
        }
        return make_node(pivot->chunk, pivot->left, std::move(new_right), output, error);
    }

    static bool rebalance(const NodePtr& root, NodePtr* output, std::string* error) {
        if (!root) {
            output->reset();
            return true;
        }
        const std::int64_t balance = static_cast<std::int64_t>(height_of(root->left)) -
                                     static_cast<std::int64_t>(height_of(root->right));
        if (balance > 1) {
            NodePtr adjusted = root;
            if (height_of(root->left->left) < height_of(root->left->right)) {
                NodePtr rotated_left_child;
                if (!rotate_left(root->left, &rotated_left_child, error) ||
                    !make_node(root->chunk, std::move(rotated_left_child), root->right, &adjusted, error)) {
                    return false;
                }
            }
            return rotate_right(adjusted, output, error);
        }
        if (balance < -1) {
            NodePtr adjusted = root;
            if (height_of(root->right->right) < height_of(root->right->left)) {
                NodePtr rotated_right_child;
                if (!rotate_right(root->right, &rotated_right_child, error) ||
                    !make_node(root->chunk, root->left, std::move(rotated_right_child), &adjusted, error)) {