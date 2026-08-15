#pragma once

#include "full_document_selection.hpp"
#include "massivedoc_store.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace zevryon::massivedoc {

enum class FullDocumentExportFormat : std::uint8_t {
    Text = 0,
    Html,
};

struct FullDocumentExportOptions {
    std::size_t cancellation_block_bytes{4096U};
};

struct FullDocumentExportStats {
    std::uint64_t records_exported{0U};
    std::uint64_t source_bytes{0U};
    std::uint64_t output_bytes{0U};
    std::size_t peak_input_block_bytes{0U};
    std::size_t peak_output_buffer_bytes{0U};
    std::size_t peak_validation_codepoints{0U};
    bool cancelled{false};
};

using FullDocumentExportCancellationCheck = std::function<bool()>;

bool export_full_document(
    const StoreReader& reader,
    const FullDocumentSelectionDescriptor& selection,
    const std::filesystem::path& output,
    FullDocumentExportFormat format,
    const FullDocumentExportCancellationCheck& cancelled,
    FullDocumentExportOptions options,
    FullDocumentExportStats* stats,
    std::string* error);

} // namespace zevryon::massivedoc
