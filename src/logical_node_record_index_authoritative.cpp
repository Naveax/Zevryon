#include "logical_node_record_index.hpp"

#include "massivedoc_positional_io.hpp"
#include "massivedoc_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace zevryon::massivedoc {
namespace {

constexpr std::size_t kHeadEntryBytes = 32U;

struct AuthoritativeHeadEntry {
    std::uint64_t first_posting{kNoLogicalNodeRecordPosting};
    std::uint64_t last_posting{kNoLogicalNodeRecordPosting};
    std::uint64_t posting_count{0U};
};

bool fail_authoritative(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

template <typename T>
T get_le(std::span<const std::uint8_t> input, std::size_t offset) {
    T value = 0U;
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        value |= static_cast<T>(input[offset + index]) << (index * 8U);
    }
    return value;
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= static_cast<std::uint32_t>(byte);
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

bool checked_table_bytes(
    std::uint64_t count,
    std::size_t width,
    std::uint64_t* output,
    std::string* error) {
    if (output == nullptr) {
        return false;
    }
    const std::uint64_t width64 = static_cast<std::uint64_t>(width);
    if (count > std::numeric_limits<std::uint64_t>::max() / width64) {
        return fail_authoritative(
            error,
            "logical-node record-index authoritative head table size overflows");
    }
    *output = count * width64;
    return true;
}

bool decode_authoritative_head(
    const BoundedPositionalReader& heads,
    const LogicalNodeRecordIndexManifest& manifest,
    std::uint64_t source_record_index,
    AuthoritativeHeadEntry* head,
    std::string* error) {
    if (head == nullptr || error == nullptr) {
        return false;
    }
    if (source_record_index >= manifest.source_record_count) {
        return fail_authoritative(
            error,
            "logical-node record-index authoritative source record is out of range");
    }
    if (source_record_index >
        std::numeric_limits<std::uint64_t>::max() /
            static_cast<std::uint64_t>(kHeadEntryBytes)) {
        return fail_authoritative(
            error,
            "logical-node record-index authoritative head offset overflows");
    }

    std::array<std::uint8_t, kHeadEntryBytes> raw{};
    const std::uint64_t offset =
        source_record_index * static_cast<std::uint64_t>(kHeadEntryBytes);
    if (!heads.read_exact_at(
            offset,
            std::span<std::byte>(
                reinterpret_cast<std::byte*>(raw.data()), raw.size()),
            error)) {
        return false;
    }
    if (get_le<std::uint32_t>(raw, 24U) != 0U ||
        crc32(std::span<const std::uint8_t>(raw.data(), 28U)) !=
            get_le<std::uint32_t>(raw, 28U)) {
        return fail_authoritative(
            error,
            "logical-node record-index authoritative head CRC/reserved mismatch");
    }

    AuthoritativeHeadEntry parsed;
    parsed.first_posting = get_le<std::uint64_t>(raw, 0U);
    parsed.last_posting = get_le<std::uint64_t>(raw, 8U);
    parsed.posting_count = get_le<std::uint64_t>(raw, 16U);

    if (parsed.posting_count == 0U) {
        if (parsed.first_posting != kNoLogicalNodeRecordPosting ||
            parsed.last_posting != kNoLogicalNodeRecordPosting) {
            return fail_authoritative(
                error,
                "logical-node record-index authoritative empty head has postings");
        }
        *head = parsed;
        return true;
    }

    if (parsed.first_posting == kNoLogicalNodeRecordPosting ||
        parsed.last_posting == kNoLogicalNodeRecordPosting) {
        return fail_authoritative(
            error,
            "logical-node record-index authoritative non-empty head misses first/last posting");
    }
    if (parsed.first_posting > parsed.last_posting) {
        return fail_authoritative(
            error,
            "logical-node record-index authoritative head first posting exceeds last posting");
    }
    if (parsed.last_posting >= manifest.posting_count) {
        return fail_authoritative(
            error,
            "logical-node record-index authoritative head last posting is out of range");
    }
    const std::uint64_t ordinal_span =
        parsed.last_posting - parsed.first_posting + 1U;
    if (parsed.posting_count > ordinal_span) {
        return fail_authoritative(
            error,
            "logical-node record-index authoritative head count exceeds posting range");
    }

    *head = parsed;
    return true;
}

} // namespace

struct LogicalNodeRecordIndexAuthoritativeReader::Impl {
    explicit Impl(std::filesystem::path value)
        : store_root(std::move(value)) {}

    std::filesystem::path store_root;
    LogicalNodeRecordIndexManifest manifest{};
    std::unique_ptr<LogicalNodeRecordIndexReader> storage;
    std::unique_ptr<BoundedPositionalReader> heads;
    bool opened{false};
};

LogicalNodeRecordIndexAuthoritativeReader::
LogicalNodeRecordIndexAuthoritativeReader(std::filesystem::path store_root)
    : impl_(std::make_unique<Impl>(std::move(store_root))) {}

LogicalNodeRecordIndexAuthoritativeReader::~LogicalNodeRecordIndexAuthoritativeReader() =
    default;
LogicalNodeRecordIndexAuthoritativeReader::
LogicalNodeRecordIndexAuthoritativeReader(
    LogicalNodeRecordIndexAuthoritativeReader&&) noexcept = default;
LogicalNodeRecordIndexAuthoritativeReader&
LogicalNodeRecordIndexAuthoritativeReader::operator=(
    LogicalNodeRecordIndexAuthoritativeReader&&) noexcept = default;

bool LogicalNodeRecordIndexAuthoritativeReader::open(std::string* error) {
    if (error == nullptr) {
        return false;
    }
    error->clear();
    if (impl_->opened) {
        return fail_authoritative(
            error,
            "logical-node record-index authoritative reader is already open");
    }

    impl_->storage =
        std::make_unique<LogicalNodeRecordIndexReader>(impl_->store_root);
    if (!impl_->storage->open(error)) {
        return false;
    }
    impl_->manifest = impl_->storage->manifest();

    impl_->heads = std::make_unique<BoundedPositionalReader>(
        impl_->store_root / "node-record-index-v1" / "heads.bin",
        kIoWindowBytes);
    std::uint64_t expected_head_bytes = 0U;
    if (!checked_table_bytes(
            impl_->manifest.source_record_count,
            kHeadEntryBytes,
            &expected_head_bytes,
            error) ||
        !impl_->heads->open(error)) {
        return false;
    }
    if (impl_->heads->file_size() != expected_head_bytes) {
        return fail_authoritative(
            error,
            "logical-node record-index authoritative head file size does not match manifest");
    }

    impl_->opened = true;
    return true;
}

const LogicalNodeRecordIndexManifest&
LogicalNodeRecordIndexAuthoritativeReader::manifest() const noexcept {
    return impl_->manifest;
}

bool LogicalNodeRecordIndexAuthoritativeReader::read_record(
    std::uint64_t source_record_index,
    std::uint64_t continuation_posting_ordinal,
    std::size_t max_nodes,
    LogicalNodeRecordIndexWindow* result,
    std::string* error) const {
    if (result == nullptr || error == nullptr) {
        return false;
    }
    *result = LogicalNodeRecordIndexWindow{};
    error->clear();
    if (!impl_->opened) {
        return fail_authoritative(
            error,
            "logical-node record-index authoritative reader is not open");
    }

    AuthoritativeHeadEntry head;
    if (!decode_authoritative_head(
            *impl_->heads,
            impl_->manifest,
            source_record_index,
            &head,
            error)) {
        return false;
    }

    const bool initial =
        continuation_posting_ordinal == kNoLogicalNodeRecordPosting;
    if (head.posting_count == 0U) {
        if (!initial) {
            return fail_authoritative(
                error,
                "logical-node record-index continuation supplied for empty head");
        }
    } else if (!initial &&
               (continuation_posting_ordinal < head.first_posting ||
                continuation_posting_ordinal > head.last_posting)) {
        return fail_authoritative(
            error,
            "logical-node record-index continuation is outside authoritative head range");
    }

    LogicalNodeRecordIndexWindow decoded;
    if (!impl_->storage->read_record(
            source_record_index,
            continuation_posting_ordinal,
            max_nodes,
            &decoded,
            error)) {
        return false;
    }

    if (head.posting_count == 0U) {
        if (!decoded.postings.empty() || decoded.truncated ||
            decoded.next_posting_ordinal != kNoLogicalNodeRecordPosting) {
            return fail_authoritative(
                error,
                "logical-node record-index empty head produced postings");
        }
        *result = std::move(decoded);
        return true;
    }

    const std::uint64_t expected_first =
        initial ? head.first_posting : continuation_posting_ordinal;
    if (decoded.postings.empty() ||
        decoded.postings.front().posting_ordinal != expected_first) {
        return fail_authoritative(
            error,
            "logical-node record-index authoritative chain start mismatch");
    }

    std::uint64_t previous = kNoLogicalNodeRecordPosting;
    for (const LogicalNodeRecordPosting& posting : decoded.postings) {
        if (posting.posting_ordinal < head.first_posting ||
            posting.posting_ordinal > head.last_posting) {
            return fail_authoritative(
                error,
                "logical-node record-index posting escapes authoritative head range");
        }
        if (previous != kNoLogicalNodeRecordPosting &&
            posting.posting_ordinal <= previous) {
            return fail_authoritative(
                error,
                "logical-node record-index authoritative postings are not strictly forward");
        }
        previous = posting.posting_ordinal;
    }

    const std::uint64_t last_emitted = decoded.postings.back().posting_ordinal;
    if (decoded.truncated) {
        if (decoded.next_posting_ordinal == kNoLogicalNodeRecordPosting ||
            decoded.next_posting_ordinal <= last_emitted ||
            decoded.next_posting_ordinal > head.last_posting) {
            return fail_authoritative(
                error,
                "logical-node record-index truncated continuation escapes authoritative head");
        }
    } else {
        if (decoded.next_posting_ordinal != kNoLogicalNodeRecordPosting ||
            last_emitted != head.last_posting) {
            return fail_authoritative(
                error,
                "logical-node record-index chain tail disagrees with authoritative head");
        }
    }

    *result = std::move(decoded);
    return true;
}

} // namespace zevryon::massivedoc
