#include "massivedoc_positional_io.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace zevryon::massivedoc {

// The record-index builder must query its staging records.bin while constructing
// posting chains, but Windows directory publication must not inherit a live
// child-file handle. Keep ordinary/final readers persistent; only the builder's
// staging record table is reopened for each bounded positional read so every
// handle is closed before the final staging-directory rename.
class RecordIndexPositionalReader final {
public:
    RecordIndexPositionalReader(
        std::filesystem::path path,
        std::size_t maximum_transfer_bytes)
        : path_(std::move(path)),
          configured_maximum_transfer_bytes_(maximum_transfer_bytes) {}

    RecordIndexPositionalReader(const RecordIndexPositionalReader&) = delete;
    RecordIndexPositionalReader& operator=(const RecordIndexPositionalReader&) = delete;

    bool open(std::string* error) {
        if (error == nullptr) {
            return false;
        }
        error->clear();
        persistent_.reset();
        opened_ = false;
        file_bytes_ = 0U;
        effective_maximum_transfer_bytes_ = 0U;
        accumulated_stats_ = PositionalIoStats{};

        transient_staging_reader_ =
            path_.filename() == "records.bin" &&
            path_.parent_path().filename() == "node-record-index-v1.building";

        auto probe = std::make_unique<BoundedPositionalReader>(
            path_, configured_maximum_transfer_bytes_);
        if (!probe->open(error)) {
            return false;
        }
        file_bytes_ = probe->file_size();
        effective_maximum_transfer_bytes_ = probe->maximum_transfer_bytes();
        opened_ = true;

        if (!transient_staging_reader_) {
            persistent_ = std::move(probe);
        }
        // In transient mode `probe` dies here, before any later directory
        // publication. Each read below acquires its own short-lived handle.
        return true;
    }

    bool read_exact_at(
        std::uint64_t offset,
        std::span<std::byte> output,
        std::string* error) const {
        if (!opened_ || error == nullptr) {
            if (error != nullptr) {
                *error = "record-index positional reader is not open";
            }
            return false;
        }
        if (!transient_staging_reader_) {
            return persistent_ != nullptr &&
                persistent_->read_exact_at(offset, output, error);
        }

        BoundedPositionalReader reader(
            path_, configured_maximum_transfer_bytes_);
        if (!reader.open(error)) {
            return false;
        }
        if (reader.file_size() != file_bytes_) {
            *error =
                "logical-node record-index staging record table changed during build";
            return false;
        }
        const bool success = reader.read_exact_at(offset, output, error);
        if (success) {
            const PositionalIoStats observed = reader.stats();
            accumulated_stats_.system_reads += observed.system_reads;
            accumulated_stats_.bytes_read += observed.bytes_read;
            accumulated_stats_.maximum_transfer_bytes = std::max(
                accumulated_stats_.maximum_transfer_bytes,
                observed.maximum_transfer_bytes);
        }
        return success;
    }

    bool is_open() const noexcept {
        return opened_;
    }

    std::uint64_t file_size() const noexcept {
        return file_bytes_;
    }

    std::size_t maximum_transfer_bytes() const noexcept {
        return effective_maximum_transfer_bytes_;
    }

    PositionalIoStats stats() const noexcept {
        return transient_staging_reader_
                   ? accumulated_stats_
                   : (persistent_ != nullptr
                          ? persistent_->stats()
                          : PositionalIoStats{});
    }

private:
    std::filesystem::path path_;
    std::size_t configured_maximum_transfer_bytes_{0U};
    std::size_t effective_maximum_transfer_bytes_{0U};
    std::uint64_t file_bytes_{0U};
    bool transient_staging_reader_{false};
    bool opened_{false};
    std::unique_ptr<BoundedPositionalReader> persistent_;
    mutable PositionalIoStats accumulated_stats_{};
};

} // namespace zevryon::massivedoc

// Compile the existing implementation unchanged, substituting only its private
// positional-reader type with the publication-safe adapter above. Public/final
// storage readers still delegate to the canonical BoundedPositionalReader.
#define BoundedPositionalReader RecordIndexPositionalReader
#include "logical_node_record_index.cpp"
#undef BoundedPositionalReader
