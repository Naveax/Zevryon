#include "full_document_export.hpp"

#include "unicode_stream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <limits>
#include <memory_resource>
#include <span>
#include <string_view>

namespace zevryon::massivedoc {
namespace {

constexpr std::size_t kValidationBlockBytes = 4096U;
constexpr std::size_t kValidationArenaBytes = 80U * 1024U;
constexpr std::size_t kHtmlOutputBufferBytes = 32U * 1024U;
constexpr std::string_view kHtmlPrefix =
    "<!doctype html><html><head><meta charset=\"utf-8\"></head><body><pre>";
constexpr std::string_view kHtmlSuffix = "</pre></body></html>";

bool add_u64(std::uint64_t value, std::uint64_t amount, std::uint64_t* output) {
    if (amount > std::numeric_limits<std::uint64_t>::max() - value) {
        return false;
    }
    *output = value + amount;
    return true;
}

bool write_raw(
    std::ofstream* stream,
    const char* data,
    std::size_t size,
    FullDocumentExportStats* stats,
    std::string* error) {
    if (size == 0U) {
        return true;
    }
    stream->write(data, static_cast<std::streamsize>(size));
    if (!*stream) {
        *error = "failed to write full-document export";
        return false;
    }
    std::uint64_t next = 0U;
    if (!add_u64(stats->output_bytes, static_cast<std::uint64_t>(size), &next)) {
        *error = "full-document export output byte count overflow";
        return false;
    }
    stats->output_bytes = next;
    return true;
}

class HtmlEscaper {
public:
    HtmlEscaper(
        std::ofstream* stream,
        FullDocumentExportStats* stats,
        std::string* error)
        : stream_(stream), stats_(stats), error_(error) {}

    bool append(std::span<const std::byte> bytes) {
        for (const std::byte value : bytes) {
            const unsigned char byte = std::to_integer<unsigned char>(value);
            switch (byte) {
                case '&':
                    if (!append_text("&amp;")) return false;
                    break;
                case '<':
                    if (!append_text("&lt;")) return false;
                    break;
                case '>':
                    if (!append_text("&gt;")) return false;
                    break;
                case '"':
                    if (!append_text("&quot;")) return false;
                    break;
                default:
                    if (!append_byte(static_cast<char>(byte))) return false;
                    break;
            }
        }
        return true;
    }

    bool append_text(std::string_view text) {
        for (const char value : text) {
            if (!append_byte(value)) {
                return false;
            }
        }
        return true;
    }

    bool flush() {
        if (used_ == 0U) {
            return true;
        }
        if (!write_raw(stream_, buffer_.data(), used_, stats_, error_)) {
            return false;
        }
        used_ = 0U;
        return true;
    }

private:
    bool append_byte(char value) {
        if (used_ == buffer_.size() && !flush()) {
            return false;
        }
        buffer_[used_++] = value;
        stats_->peak_output_buffer_bytes =
            std::max(stats_->peak_output_buffer_bytes, used_);
        return true;
    }

    std::ofstream* stream_;
    FullDocumentExportStats* stats_;
    std::string* error_;
    std::array<char, kHtmlOutputBufferBytes> buffer_{};
    std::size_t used_{0U};
};

void remove_if_exists(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

bool publish_export(
    const std::filesystem::path& temporary,
    const std::filesystem::path& output,
    std::string* error) {
    std::error_code fs_error;
    if (!std::filesystem::exists(output, fs_error)) {
        fs_error.clear();
        std::filesystem::rename(temporary, output, fs_error);
        if (!fs_error) {
            return true;
        }
        *error = "cannot publish full-document export: " + fs_error.message();
        return false;
    }
    if (fs_error) {
        *error = "cannot inspect export target: " + fs_error.message();
        return false;
    }

    std::filesystem::path backup = output;
    backup += ".zevryon-backup";
    remove_if_exists(backup);
    std::filesystem::rename(output, backup, fs_error);
    if (fs_error) {
        *error = "cannot preserve existing export target: " + fs_error.message();
        return false;
    }
    fs_error.clear();
    std::filesystem::rename(temporary, output, fs_error);
    if (fs_error) {
        std::error_code restore_error;
        std::filesystem::rename(backup, output, restore_error);
        *error = "cannot publish full-document export: " + fs_error.message();
        return false;
    }
    remove_if_exists(backup);
    return true;
}

bool descriptor_is_full_document(
    const FullDocumentSelectionDescriptor& selection) noexcept {
    const SequenceAggregate aggregate = selection.snapshot.stats().aggregate;
    return selection.start_text_offset == 0U &&
        selection.end_text_offset == aggregate.text_bytes &&
        selection.record_count == aggregate.record_count;
}

} // namespace

bool export_full_document(
    const StoreReader& reader,
    const FullDocumentSelectionDescriptor& selection,
    const std::filesystem::path& output,
    FullDocumentExportFormat format,
    const FullDocumentExportCancellationCheck& cancelled,
    FullDocumentExportOptions options,
    FullDocumentExportStats* stats,
    std::string* error) {
    FullDocumentExportStats local_stats;
    FullDocumentExportStats* const active_stats = stats != nullptr ? stats : &local_stats;
    *active_stats = {};
    error->clear();

    if (options.cancellation_block_bytes == 0U) {
        *error = "full-document export cancellation block must be non-zero";
        return false;
    }
    if (!descriptor_is_full_document(selection)) {
        *error = "selection descriptor does not cover its complete immutable snapshot";
        return false;
    }
    if (selection.record_count != reader.stats().corpus.logical_records) {
        *error = "selection/store record count mismatch";
        return false;
    }

    std::filesystem::path temporary = output;
    temporary += ".zevryon-tmp";
    remove_if_exists(temporary);
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        *error = "cannot create full-document export temporary file";
        return false;
    }

    const auto fail_and_cleanup = [&stream, &temporary]() {
        stream.close();
        remove_if_exists(temporary);
    };

    HtmlEscaper html(&stream, active_stats, error);
    if (format == FullDocumentExportFormat::Html && !html.append_text(kHtmlPrefix)) {
        fail_and_cleanup();
        return false;
    }

    std::array<std::byte, kValidationArenaBytes> validation_arena{};
    std::pmr::monotonic_buffer_resource validation_resource(
        validation_arena.data(),
        validation_arena.size(),
        std::pmr::null_memory_resource());
    std::pmr::vector<text::DecodedCodePoint> decoded(&validation_resource);
    try {
        decoded.reserve(kValidationBlockBytes + 1U);
    } catch (...) {
        *error = "cannot allocate bounded HTML UTF-8 validation buffer";
        fail_and_cleanup();
        return false;
    }
    text::Utf8StreamDecoder decoder(text::Utf8ErrorPolicy::Strict);

    for (std::uint64_t logical_index = 0U;
         logical_index < selection.record_count;
         ++logical_index) {
        if (cancelled && cancelled()) {
            active_stats->cancelled = true;
            *error = "export cancelled";
            fail_and_cleanup();
            return false;
        }

        SequencePosition position;
        std::string sequence_error;
        if (!selection.snapshot.at(logical_index, &position, &sequence_error)) {
            *error = "cannot resolve logical export record: " + sequence_error;
            fail_and_cleanup();
            return false;
        }
        if (position.record.source_record_index >= reader.stats().corpus.logical_records) {
            *error = "logical export source record index out of range";
            fail_and_cleanup();
            return false;
        }

        decoder.reset();
        std::uint64_t record_bytes = 0U;
        bool callback_ok = true;
        bool callback_cancelled = false;
        std::string callback_error;
        const bool read_ok = reader.read_record(
            position.record.source_record_index,
            [&](std::span<const std::byte> bytes) {
                std::size_t offset = 0U;
                while (offset < bytes.size()) {
                    if (cancelled && cancelled()) {
                        callback_cancelled = true;
                        return false;
                    }
                    const std::size_t amount = std::min(
                        options.cancellation_block_bytes,
                        bytes.size() - offset);
                    const auto block = bytes.subspan(offset, amount);
                    active_stats->peak_input_block_bytes =
                        std::max(active_stats->peak_input_block_bytes, amount);

                    if (format == FullDocumentExportFormat::Html) {
                        std::size_t validation_offset = 0U;
                        while (validation_offset < block.size()) {
                            const std::size_t validation_amount = std::min(
                                kValidationBlockBytes,
                                block.size() - validation_offset);
                            decoded.clear();
                            text::Utf8DecodeError decode_error;
                            if (!decoder.feed(
                                    block.subspan(validation_offset, validation_amount),
                                    record_bytes + static_cast<std::uint64_t>(validation_offset),
                                    &decoded,
                                    &decode_error)) {
                                callback_error = "HTML export requires valid UTF-8 at source byte " +
                                    std::to_string(decode_error.source_offset);
                                callback_ok = false;
                                return false;
                            }
                            active_stats->peak_validation_codepoints =
                                std::max(active_stats->peak_validation_codepoints, decoded.size());
                            validation_offset += validation_amount;
                        }
                        if (!html.append(block)) {
                            callback_error = *error;
                            callback_ok = false;
                            return false;
                        }
                    } else {
                        if (!write_raw(
                                &stream,
                                reinterpret_cast<const char*>(block.data()),
                                block.size(),
                                active_stats,
                                &callback_error)) {
                            callback_ok = false;
                            return false;
                        }
                    }

                    std::uint64_t next_record_bytes = 0U;
                    if (!add_u64(
                            record_bytes,
                            static_cast<std::uint64_t>(amount),
                            &next_record_bytes)) {
                        callback_error = "logical export record byte count overflow";
                        callback_ok = false;
                        return false;
                    }
                    record_bytes = next_record_bytes;
                    offset += amount;
                }
                return true;
            },
            error);

        if (!read_ok) {
            fail_and_cleanup();
            return false;
        }
        if (callback_cancelled) {
            active_stats->cancelled = true;
            *error = "export cancelled";
            fail_and_cleanup();
            return false;
        }
        if (!callback_ok) {
            *error = callback_error;
            fail_and_cleanup();
            return false;
        }
        if (format == FullDocumentExportFormat::Html) {
            decoded.clear();
            text::Utf8DecodeError decode_error;
            if (!decoder.finish(&decoded, &decode_error)) {
                *error = "HTML export requires valid UTF-8 at source byte " +
                    std::to_string(decode_error.source_offset);
                fail_and_cleanup();
                return false;
            }
            active_stats->peak_validation_codepoints =
                std::max(active_stats->peak_validation_codepoints, decoded.size());
        }
        if (record_bytes != position.record.text_bytes) {
            *error = "logical export record byte metadata mismatch";
            fail_and_cleanup();
            return false;
        }

        std::uint64_t next_source_bytes = 0U;
        if (!add_u64(active_stats->source_bytes, record_bytes, &next_source_bytes)) {
            *error = "full-document export source byte count overflow";
            fail_and_cleanup();
            return false;
        }
        active_stats->source_bytes = next_source_bytes;
        ++active_stats->records_exported;
    }

    if (active_stats->source_bytes != selection.text_bytes()) {
        *error = "full-document export total byte metadata mismatch";
        fail_and_cleanup();
        return false;
    }
    if (format == FullDocumentExportFormat::Html) {
        if (!html.append_text(kHtmlSuffix) || !html.flush()) {
            fail_and_cleanup();
            return false;
        }
    }
    stream.flush();
    if (!stream) {
        *error = "failed to flush full-document export";
        fail_and_cleanup();
        return false;
    }
    stream.close();
    if (!stream) {
        *error = "failed to close full-document export";
        remove_if_exists(temporary);
        return false;
    }
    if (!publish_export(temporary, output, error)) {
        remove_if_exists(temporary);
        return false;
    }
    return true;
}

} // namespace zevryon::massivedoc
