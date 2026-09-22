#include "css_inline_cascade_merge_v1.hpp"

#include <limits>
#include <new>
#include <string_view>
#include <vector>

namespace zevryon::style {
namespace {

struct ResolvedWinnerV1 {
    std::string_view property;
    std::string_view value;
    bool important{false};
    bool inline_origin{false};
};

bool set_error(
    CssInlineCascadeMergeErrorV1* error,
    CssInlineCascadeMergeErrorKindV1 kind,
    std::size_t index,
    std::string_view message) noexcept {
    if (error != nullptr) {
        error->kind = kind;
        error->index = index;
        try {
            error->message.assign(message.data(), message.size());
        } catch (...) {
            error->message.clear();
        }
    }
    return false;
}

bool slice_valid(
    const CssStylesheetV1& stylesheet,
    CssTextSliceV1 slice) noexcept {
    const std::size_t offset =
        static_cast<std::size_t>(slice.offset);
    const std::size_t length =
        static_cast<std::size_t>(slice.length);
    return offset <= stylesheet.text.size() &&
        length <= stylesheet.text.size() - offset;
}

bool consume_work(
    CssInlineCascadeMergeConfigV1 config,
    CssInlineCascadeMergeStatsV1* stats,
    std::uint64_t units) noexcept {
    if (stats == nullptr ||
        stats->work_units > config.maximum_work_units ||
        units > config.maximum_work_units - stats->work_units) {
        return false;
    }
    stats->work_units += units;
    return true;
}

bool properties_equal(
    std::string_view left,
    std::string_view right,
    CssInlineCascadeMergeConfigV1 config,
    CssInlineCascadeMergeStatsV1* stats,
    bool* equal) noexcept {
    if (equal == nullptr) {
        return false;
    }
    const std::uint64_t left_bytes =
        static_cast<std::uint64_t>(left.size());
    const std::uint64_t right_bytes =
        static_cast<std::uint64_t>(right.size());
    if (left_bytes >
        std::numeric_limits<std::uint64_t>::max() -
            right_bytes) {
        return false;
    }
    const std::uint64_t sum = left_bytes + right_bytes;
    if (sum == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    if (!consume_work(config, stats, sum + 1U)) {
        return false;
    }
    *equal = left == right;
    return true;
}

bool append_text_slice(
    std::string_view value,
    CssInlineCascadeMergeConfigV1 config,
    CssStylesheetV1* stylesheet,
    CssInlineCascadeMergeStatsV1* stats,
    CssTextSliceV1* slice) {
    if (stylesheet == nullptr || stats == nullptr ||
        slice == nullptr) {
        return false;
    }
    if (stylesheet->text.size() >
            config.maximum_output_text_bytes ||
        value.size() >
            config.maximum_output_text_bytes -
                stylesheet->text.size()) {
        return false;
    }
    if (stylesheet->text.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()) ||
        value.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    slice->offset =
        static_cast<std::uint32_t>(stylesheet->text.size());
    slice->length =
        static_cast<std::uint32_t>(value.size());
    stylesheet->text.append(value.data(), value.size());
    stats->output_text_bytes +=
        static_cast<std::uint64_t>(value.size());
    return true;
}

} // namespace

bool CssInlineCascadeMergeConfigV1::valid() const noexcept {
    return maximum_author_properties > 0U &&
        maximum_author_properties <=
            kMaximumAuthorPropertiesLimit &&
        maximum_inline_declarations > 0U &&
        maximum_inline_declarations <=
            kMaximumInlineDeclarationsLimit &&
        maximum_output_properties > 0U &&
        maximum_output_properties <=
            kMaximumOutputPropertiesLimit &&
        maximum_output_text_bytes > 0U &&
        maximum_output_text_bytes <=
            kMaximumOutputTextBytesLimit &&
        maximum_output_text_bytes <=
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()) &&
        maximum_work_units > 0U &&
        maximum_work_units <= kMaximumWorkUnitsLimit;
}

CssInlineMergedCascadeV1::CssInlineMergedCascadeV1(
    std::pmr::memory_resource* memory)
    : stylesheet(memory),
      cascade(memory) {}

std::pmr::memory_resource*
CssInlineMergedCascadeV1::resource() const noexcept {
    return stylesheet.resource();
}

void CssInlineMergedCascadeV1::release() noexcept {
    stylesheet.release();
    cascade.release();
}

const char* css_inline_cascade_merge_error_kind_name_v1(
    CssInlineCascadeMergeErrorKindV1 kind) noexcept {
    switch (kind) {
    case CssInlineCascadeMergeErrorKindV1::None:
        return "none";
    case CssInlineCascadeMergeErrorKindV1::InvalidConfiguration:
        return "invalid-configuration";
    case CssInlineCascadeMergeErrorKindV1::AuthorPropertyLimitExceeded:
        return "author-property-limit-exceeded";
    case CssInlineCascadeMergeErrorKindV1::InlineDeclarationLimitExceeded:
        return "inline-declaration-limit-exceeded";
    case CssInlineCascadeMergeErrorKindV1::OutputPropertyLimitExceeded:
        return "output-property-limit-exceeded";
    case CssInlineCascadeMergeErrorKindV1::TextBudgetExceeded:
        return "text-budget-exceeded";
    case CssInlineCascadeMergeErrorKindV1::WorkBudgetExceeded:
        return "work-budget-exceeded";
    case CssInlineCascadeMergeErrorKindV1::InvalidAuthorCascade:
        return "invalid-author-cascade";
    case CssInlineCascadeMergeErrorKindV1::InvalidInlineDeclaration:
        return "invalid-inline-declaration";
    case CssInlineCascadeMergeErrorKindV1::RepresentationOverflow:
        return "representation-overflow";
    case CssInlineCascadeMergeErrorKindV1::AllocationFailure:
        return "allocation-failure";
    }
    return "unknown";
}

bool merge_css_author_and_inline_cascade_v1(
    const CssStylesheetV1& author_stylesheet,
    const CssCascadeResultV1& author_cascade,
    const CssStylesheetV1& inline_stylesheet,
    std::span<const CssDeclarationV1> inline_declarations,
    CssInlineCascadeMergeConfigV1 config,
    CssInlineMergedCascadeV1* output,
    CssInlineCascadeMergeStatsV1* stats,
    CssInlineCascadeMergeErrorV1* error) noexcept {
    if (stats != nullptr) {
        *stats = CssInlineCascadeMergeStatsV1{};
    }
    if (error != nullptr) {
        *error = CssInlineCascadeMergeErrorV1{};
    }
    if (output == nullptr || stats == nullptr ||
        error == nullptr || output->resource() == nullptr ||
        !config.valid()) {
        return set_error(
            error,
            CssInlineCascadeMergeErrorKindV1::InvalidConfiguration,
            0U,
            "CSS inline cascade merge requires output, stats, error and valid configuration");
    }
    if (author_cascade.winners.size() >
        config.maximum_author_properties) {
        return set_error(
            error,
            CssInlineCascadeMergeErrorKindV1::AuthorPropertyLimitExceeded,
            config.maximum_author_properties,
            "author cascade property count exceeds configured merge limit");
    }
    if (inline_declarations.size() >
        config.maximum_inline_declarations) {
        return set_error(
            error,
            CssInlineCascadeMergeErrorKindV1::InlineDeclarationLimitExceeded,
            config.maximum_inline_declarations,
            "inline declaration count exceeds configured merge limit");
    }

    CssInlineCascadeMergeStatsV1 candidate_stats;
    CssInlineMergedCascadeV1 candidate(output->resource());

    try {
        std::pmr::vector<ResolvedWinnerV1> winners(
            output->resource());
        winners.reserve(
            author_cascade.winners.size() +
            inline_declarations.size());

        for (std::size_t index = 0U;
             index < author_cascade.winners.size();
             ++index) {
            const CssCascadeWinnerV1& winner =
                author_cascade.winners[index];
            ++candidate_stats.author_properties;
            if (!consume_work(
                    config,
                    &candidate_stats,
                    1U)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssInlineCascadeMergeErrorKindV1::WorkBudgetExceeded,
                    index,
                    "author cascade validation exceeded inline merge work budget");
            }
            if (!slice_valid(
                    author_stylesheet,
                    winner.property) ||
                !slice_valid(
                    author_stylesheet,
                    winner.value) ||
                winner.property.length == 0U) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssInlineCascadeMergeErrorKindV1::InvalidAuthorCascade,
                    index,
                    "author cascade winner contains invalid property/value slices");
            }

            const std::string_view property =
                author_stylesheet.resolve(winner.property);
            for (std::size_t prior = 0U;
                 prior < winners.size();
                 ++prior) {
                bool equal = false;
                if (!properties_equal(
                        property,
                        winners[prior].property,
                        config,
                        &candidate_stats,
                        &equal)) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssInlineCascadeMergeErrorKindV1::WorkBudgetExceeded,
                        index,
                        "author cascade duplicate-property validation exceeded work budget");
                }
                if (equal) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssInlineCascadeMergeErrorKindV1::InvalidAuthorCascade,
                        index,
                        "author cascade contains duplicate winning property");
                }
            }

            winners.push_back(
                ResolvedWinnerV1{
                    property,
                    author_stylesheet.resolve(winner.value),
                    winner.important,
                    false});
        }

        for (std::size_t index = 0U;
             index < inline_declarations.size();
             ++index) {
            const CssDeclarationV1& declaration =
                inline_declarations[index];
            ++candidate_stats.inline_declarations;
            if (!consume_work(
                    config,
                    &candidate_stats,
                    1U)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssInlineCascadeMergeErrorKindV1::WorkBudgetExceeded,
                    index,
                    "inline declaration scan exceeded merge work budget");
            }
            if (!slice_valid(
                    inline_stylesheet,
                    declaration.property) ||
                !slice_valid(
                    inline_stylesheet,
                    declaration.value) ||
                declaration.property.length == 0U) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssInlineCascadeMergeErrorKindV1::InvalidInlineDeclaration,
                    index,
                    "inline declaration contains invalid property/value slices");
            }

            const std::string_view property =
                inline_stylesheet.resolve(
                    declaration.property);
            const std::string_view value =
                inline_stylesheet.resolve(
                    declaration.value);

            std::size_t found = winners.size();
            for (std::size_t existing = 0U;
                 existing < winners.size();
                 ++existing) {
                bool equal = false;
                if (!properties_equal(
                        property,
                        winners[existing].property,
                        config,
                        &candidate_stats,
                        &equal)) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssInlineCascadeMergeErrorKindV1::WorkBudgetExceeded,
                        index,
                        "inline property lookup exceeded merge work budget");
                }
                if (equal) {
                    found = existing;
                    break;
                }
            }

            if (found == winners.size()) {
                if (winners.size() >=
                    config.maximum_output_properties) {
                    *stats = candidate_stats;
                    return set_error(
                        error,
                        CssInlineCascadeMergeErrorKindV1::OutputPropertyLimitExceeded,
                        index,
                        "merged inline cascade exceeds output property limit");
                }
                winners.push_back(
                    ResolvedWinnerV1{
                        property,
                        value,
                        declaration.important,
                        true});
                continue;
            }

            ResolvedWinnerV1& current =
                winners[found];
            if (current.inline_origin) {
                ++candidate_stats.inline_duplicate_declarations;
                const bool replace =
                    declaration.important != current.important
                        ? declaration.important
                        : true;
                if (replace) {
                    current.value = value;
                    current.important =
                        declaration.important;
                    ++candidate_stats.inline_duplicate_replacements;
                }
                continue;
            }

            if (declaration.important !=
                current.important) {
                if (declaration.important) {
                    current.value = value;
                    current.important = true;
                    current.inline_origin = true;
                    ++candidate_stats.inline_overrides_author;
                } else {
                    ++candidate_stats.author_important_preserved;
                }
                continue;
            }

            current.value = value;
            current.important =
                declaration.important;
            current.inline_origin = true;
            ++candidate_stats.inline_overrides_author;
        }

        if (winners.size() >
            config.maximum_output_properties) {
            *stats = candidate_stats;
            return set_error(
                error,
                CssInlineCascadeMergeErrorKindV1::OutputPropertyLimitExceeded,
                config.maximum_output_properties,
                "merged inline cascade exceeds output property limit");
        }

        candidate.cascade.winners.reserve(
            winners.size());
        for (std::size_t index = 0U;
             index < winners.size();
             ++index) {
            const ResolvedWinnerV1& winner =
                winners[index];

            const std::uint64_t property_bytes =
                static_cast<std::uint64_t>(
                    winner.property.size());
            const std::uint64_t value_bytes =
                static_cast<std::uint64_t>(
                    winner.value.size());
            if (property_bytes >
                std::numeric_limits<std::uint64_t>::max() -
                    value_bytes) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssInlineCascadeMergeErrorKindV1::RepresentationOverflow,
                    index,
                    "merged winner copy work overflows 64-bit accounting");
            }
            const std::uint64_t copy_sum =
                property_bytes + value_bytes;
            if (copy_sum ==
                std::numeric_limits<std::uint64_t>::max() ||
                !consume_work(
                    config,
                    &candidate_stats,
                    copy_sum + 1U)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssInlineCascadeMergeErrorKindV1::WorkBudgetExceeded,
                    index,
                    "merged winner publication exceeded work budget");
            }

            if (candidate.stylesheet.text.size() >
                    config.maximum_output_text_bytes ||
                winner.property.size() >
                    config.maximum_output_text_bytes -
                        candidate.stylesheet.text.size()) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssInlineCascadeMergeErrorKindV1::TextBudgetExceeded,
                    index,
                    "merged property exceeds output text budget");
            }

            CssTextSliceV1 property_slice;
            if (!append_text_slice(
                    winner.property,
                    config,
                    &candidate.stylesheet,
                    &candidate_stats,
                    &property_slice)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssInlineCascadeMergeErrorKindV1::RepresentationOverflow,
                    index,
                    "merged property exceeds 32-bit text representation");
            }

            if (candidate.stylesheet.text.size() >
                    config.maximum_output_text_bytes ||
                winner.value.size() >
                    config.maximum_output_text_bytes -
                        candidate.stylesheet.text.size()) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssInlineCascadeMergeErrorKindV1::TextBudgetExceeded,
                    index,
                    "merged value exceeds output text budget");
            }

            CssTextSliceV1 value_slice;
            if (!append_text_slice(
                    winner.value,
                    config,
                    &candidate.stylesheet,
                    &candidate_stats,
                    &value_slice)) {
                *stats = candidate_stats;
                return set_error(
                    error,
                    CssInlineCascadeMergeErrorKindV1::RepresentationOverflow,
                    index,
                    "merged value exceeds 32-bit text representation");
            }

            candidate.cascade.winners.push_back(
                CssCascadeWinnerV1{
                    property_slice,
                    value_slice,
                    winner.important,
                    CssSpecificityV1{},
                    0U});
        }

        candidate_stats.output_properties =
            static_cast<std::uint64_t>(
                candidate.cascade.winners.size());

        output->release();
        output->stylesheet.text.swap(
            candidate.stylesheet.text);
        output->stylesheet.rules.swap(
            candidate.stylesheet.rules);
        output->stylesheet.declarations.swap(
            candidate.stylesheet.declarations);
        output->cascade.winners.swap(
            candidate.cascade.winners);
        *stats = candidate_stats;
        return true;
    } catch (const std::bad_alloc&) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssInlineCascadeMergeErrorKindV1::AllocationFailure,
            0U,
            "CSS inline cascade merge allocation failed");
    } catch (...) {
        *stats = candidate_stats;
        return set_error(
            error,
            CssInlineCascadeMergeErrorKindV1::AllocationFailure,
            0U,
            "CSS inline cascade merge container operation failed");
    }
}

} // namespace zevryon::style
