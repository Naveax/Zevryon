#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace zevryon::text {

struct SearchSourceCodePoint {
    std::uint32_t value{0};
    std::uint64_t source_start{0};
    std::uint64_t source_end{0};
};

struct NormalizedSearchCodePoint {
    std::uint32_t value{0};
    std::uint64_t source_start{0};
    std::uint64_t source_end{0};
};

struct UnicodeSearchMapEntry {
    std::uint32_t codepoint{0};
    std::uint32_t offset{0};
    std::uint16_t length{0};
};

struct UnicodeSearchCombiningClassRange {
    std::uint32_t first{0};
    std::uint32_t last{0};
    std::uint8_t combining_class{0};
};

struct UnicodeSearchCompositionEntry {
    std::uint32_t first{0};
    std::uint32_t second{0};
    std::uint32_t composite{0};
};

struct UnicodeSearchNormalizationTables {
    std::string_view unicode_version;
    std::string_view fingerprint;
    std::span<const UnicodeSearchMapEntry> canonical_decomposition;
    std::span<const std::uint32_t> canonical_decomposition_pool;
    std::span<const UnicodeSearchMapEntry> full_case_fold;
    std::span<const std::uint32_t> full_case_fold_pool;
    std::span<const UnicodeSearchCombiningClassRange> combining_classes;
    std::span<const UnicodeSearchCompositionEntry> compositions;
};

struct UnicodeSearchNormalizerConfig {
    std::size_t max_pending_codepoints{256U};
    bool full_case_fold{true};
    bool compose{true};
};

enum class UnicodeSearchNormalizationErrorKind : std::uint8_t {
    None = 0,
    InvalidConfiguration,
    InvalidInput,
    InvalidTable,
    PendingSequenceLimitExceeded,
    ConsumerStopped,
    AlreadyFinished,
};

struct UnicodeSearchNormalizationError {
    UnicodeSearchNormalizationErrorKind kind{UnicodeSearchNormalizationErrorKind::None};
    std::uint64_t source_offset{0};
    std::string_view message;
};

struct UnicodeSearchNormalizationStats {
    std::uint64_t input_codepoints{0};
    std::uint64_t canonical_decomposition_expansions{0};
    std::uint64_t case_fold_expansions{0};
    std::uint64_t emitted_codepoints{0};
    std::uint64_t emitted_segments{0};
    std::size_t peak_pending_codepoints{0};
};

using UnicodeSearchNormalizationConsumer =
    std::function<bool(std::span<const NormalizedSearchCodePoint>)>;

bool validate_unicode_search_normalization_tables(
    const UnicodeSearchNormalizationTables& tables) noexcept;

class UnicodeSearchNormalizer {
public:
    UnicodeSearchNormalizer(
        const UnicodeSearchNormalizationTables& tables,
        UnicodeSearchNormalizerConfig config = {});

    bool feed(
        std::span<const SearchSourceCodePoint> input,
        const UnicodeSearchNormalizationConsumer& consumer,
        UnicodeSearchNormalizationError* error);

    bool finish(
        const UnicodeSearchNormalizationConsumer& consumer,
        UnicodeSearchNormalizationError* error);

    void reset() noexcept;

    const UnicodeSearchNormalizationStats& stats() const noexcept;
    bool finished() const noexcept;
    bool failed() const noexcept;

private:
    struct PendingCodePoint {
        std::uint32_t value{0};
        std::uint64_t source_start{0};
        std::uint64_t source_end{0};
        std::uint8_t combining_class{0};
    };

    bool fail(
        UnicodeSearchNormalizationErrorKind kind,
        std::uint64_t source_offset,
        std::string_view message,
        UnicodeSearchNormalizationError* error) noexcept;

    bool process_source(
        const SearchSourceCodePoint& source,
        const UnicodeSearchNormalizationConsumer& consumer,
        UnicodeSearchNormalizationError* error);

    bool append_decomposed(
        std::uint32_t value,
        std::uint64_t source_start,
        std::uint64_t source_end,
        const UnicodeSearchNormalizationConsumer& consumer,
        UnicodeSearchNormalizationError* error);

    bool flush_pending(
        const UnicodeSearchNormalizationConsumer& consumer,
        UnicodeSearchNormalizationError* error);

    void prepare_pending_output();

    bool append_pending(
        std::uint32_t value,
        std::uint64_t source_start,
        std::uint64_t source_end,
        std::uint8_t combining_class,
        UnicodeSearchNormalizationError* error);

    const UnicodeSearchMapEntry* find_map_entry(
        std::span<const UnicodeSearchMapEntry> entries,
        std::uint32_t codepoint) const noexcept;

    std::uint8_t combining_class(std::uint32_t codepoint) const noexcept;
    std::uint32_t compose_pair(std::uint32_t first, std::uint32_t second) const noexcept;

    bool visit_decomposition(
        std::uint32_t codepoint,
        const std::function<bool(std::uint32_t)>& visitor,
        UnicodeSearchNormalizationError* error,
        std::uint64_t source_offset) const;

    bool visit_case_fold(
        std::uint32_t codepoint,
        const std::function<bool(std::uint32_t)>& visitor,
        UnicodeSearchNormalizationError* error,
        std::uint64_t source_offset) const;

    static bool decompose_hangul(
        std::uint32_t codepoint,
        std::uint32_t* output,
        std::size_t* length) noexcept;

    static std::uint32_t compose_hangul(
        std::uint32_t first,
        std::uint32_t second) noexcept;

    const UnicodeSearchNormalizationTables& tables_;
    UnicodeSearchNormalizerConfig config_;
    UnicodeSearchNormalizationStats stats_{};
    std::vector<PendingCodePoint> pending_;
    std::vector<NormalizedSearchCodePoint> emit_buffer_;
    bool tables_valid_{false};
    bool finished_{false};
    bool failed_{false};
};

} // namespace zevryon::text
