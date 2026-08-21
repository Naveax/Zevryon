#include "shared_record_length_authority.hpp"

#include <algorithm>
#include <limits>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace zevryon::massivedoc {
namespace {

std::uint64_t saturating_increment(std::uint64_t value) noexcept {
    return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1U;
}

struct RecordLengthKey {
    std::string root;
    std::uint64_t record_index{0U};

    bool operator==(const RecordLengthKey&) const noexcept = default;
};

struct RecordLengthKeyHash {
    std::size_t operator()(const RecordLengthKey& key) const noexcept {
        const std::size_t root_hash = std::hash<std::string>{}(key.root);
        const std::size_t record_hash = static_cast<std::size_t>(
            key.record_index ^ (key.record_index >> 32U));
        return root_hash ^ (record_hash + static_cast<std::size_t>(0x9e3779b9U) +
                            (root_hash << 6U) + (root_hash >> 2U));
    }
};

bool make_key(
    const std::filesystem::path& root,
    std::uint64_t record_index,
    std::size_t max_root_key_bytes,
    RecordLengthKey* key,
    std::string* error) {
    if (key == nullptr || error == nullptr) {
        return false;
    }
    try {
        std::string normalized = root.lexically_normal().generic_string();
        if (normalized.empty() || normalized.size() > max_root_key_bytes) {
            *error = "record-length store key is empty or exceeds configured bound";
            return false;
        }
        *key = RecordLengthKey{std::move(normalized), record_index};
        return true;
    } catch (...) {
        *error = "unable to materialize bounded record-length store key";
        return false;
    }
}

} // namespace

bool SharedRecordLengthAuthorityConfig::valid() const noexcept {
    return max_entries > 0U && max_root_key_bytes > 0U &&
           max_root_key_bytes <= 64U * 1024U;
}

struct SharedRecordLengthAuthority::State {
    struct Entry {
        std::uint64_t record_length{0U};
        std::list<RecordLengthKey>::iterator lru_position;
    };

    explicit State(SharedRecordLengthAuthorityConfig config_value)
        : config(config_value) {}

    void touch_locked(
        std::unordered_map<RecordLengthKey, Entry, RecordLengthKeyHash>::iterator found) {
        lru.splice(lru.begin(), lru, found->second.lru_position);
        found->second.lru_position = lru.begin();
    }

    void evict_one_locked() {
        if (lru.empty()) {
            return;
        }
        const RecordLengthKey key = lru.back();
        lru.pop_back();
        cache.erase(key);
        status.evictions = saturating_increment(status.evictions);
    }

    bool insert_locked(
        RecordLengthKey key,
        std::uint64_t record_length,
        bool count_replacement) {
        auto found = cache.find(key);
        if (found != cache.end()) {
            found->second.record_length = record_length;
            touch_locked(found);
            if (count_replacement) {
                status.replacements = saturating_increment(status.replacements);
            }
            return true;
        }
        while (cache.size() >= config.max_entries) {
            evict_one_locked();
        }
        try {
            lru.push_front(key);
            Entry entry{record_length, lru.begin()};
            const auto inserted = cache.emplace(std::move(key), std::move(entry));
            if (!inserted.second) {
                lru.pop_front();
                return false;
            }
        } catch (...) {
            if (!lru.empty() && cache.find(lru.front()) == cache.end()) {
                lru.pop_front();
            }
            return false;
        }
        status.insertions = saturating_increment(status.insertions);
        status.entries = cache.size();
        status.peak_entries = std::max(status.peak_entries, status.entries);
        return true;
    }

    SharedRecordLengthAuthorityConfig config;
    mutable std::mutex mutex;
    std::list<RecordLengthKey> lru;
    std::unordered_map<RecordLengthKey, Entry, RecordLengthKeyHash> cache;
    SharedRecordLengthAuthorityStatus status;
};

SharedRecordLengthAuthority::SharedRecordLengthAuthority(
    SharedRecordLengthAuthorityConfig config)
    : state_(std::make_unique<State>(config)) {}

SharedRecordLengthAuthority::~SharedRecordLengthAuthority() = default;

bool SharedRecordLengthAuthority::valid() const noexcept {
    return state_ != nullptr && state_->config.valid();
}

bool SharedRecordLengthAuthority::query(
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    const RecordLengthResolver& resolver,
    std::uint64_t* record_length,
    std::string* error) {
    if (!valid() || !resolver || record_length == nullptr || error == nullptr) {
        if (state_ != nullptr) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->status.invalid_requests =
                saturating_increment(state_->status.invalid_requests);
        }
        if (error != nullptr) {
            *error = "invalid shared record-length query";
        }
        return false;
    }
    error->clear();
    RecordLengthKey key;
    if (!make_key(
            store_root,
            record_index,
            state_->config.max_root_key_bytes,
            &key,
            error)) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->status.invalid_requests =
            saturating_increment(state_->status.invalid_requests);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto found = state_->cache.find(key);
        if (found != state_->cache.end()) {
            *record_length = found->second.record_length;
            state_->touch_locked(found);
            state_->status.cache_hits = saturating_increment(state_->status.cache_hits);
            return true;
        }
        state_->status.cache_misses = saturating_increment(state_->status.cache_misses);
    }

    std::uint64_t resolved = 0U;
    std::string resolver_error;
    if (!resolver(store_root, record_index, &resolved, &resolver_error)) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->status.resolver_failures =
            saturating_increment(state_->status.resolver_failures);
        *error = resolver_error.empty()
                     ? "record-length resolver failed without diagnostic"
                     : std::move(resolver_error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto found = state_->cache.find(key);
        if (found != state_->cache.end()) {
            *record_length = found->second.record_length;
            state_->touch_locked(found);
            state_->status.cache_hits = saturating_increment(state_->status.cache_hits);
            return true;
        }
        if (!state_->insert_locked(std::move(key), resolved, false)) {
            *error = "unable to admit record length into bounded authority cache";
            return false;
        }
    }
    *record_length = resolved;
    return true;
}

bool SharedRecordLengthAuthority::remember(
    const std::filesystem::path& store_root,
    std::uint64_t record_index,
    std::uint64_t record_length,
    std::string* error) {
    if (!valid() || error == nullptr) {
        if (error != nullptr) {
            *error = "invalid shared record-length remember request";
        }
        return false;
    }
    error->clear();
    RecordLengthKey key;
    if (!make_key(
            store_root,
            record_index,
            state_->config.max_root_key_bytes,
            &key,
            error)) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->status.invalid_requests =
            saturating_increment(state_->status.invalid_requests);
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->insert_locked(std::move(key), record_length, true)) {
        *error = "unable to remember record length in bounded authority cache";
        return false;
    }
    return true;
}

SharedRecordLengthAuthorityStatus SharedRecordLengthAuthority::status() const {
    if (state_ == nullptr) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    SharedRecordLengthAuthorityStatus snapshot = state_->status;
    snapshot.entries = state_->cache.size();
    return snapshot;
}

void SharedRecordLengthAuthority::clear() noexcept {
    if (state_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->cache.clear();
    state_->lru.clear();
    state_->status.entries = 0U;
}

} // namespace zevryon::massivedoc
