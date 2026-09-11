#include "html_tokenizer_data_stream_v1.hpp"

#include "html_tokenizer_data_tags_v1.hpp"
#include "html_tokenizer_markup_declarations_v1.hpp"
#include "html_tokenizer_utf8_v1.hpp"

#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace zevryon::massivedoc {
namespace {

constexpr std::size_t kMaximumConfiguredInputBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumConfiguredTokenBytes = 1024U * 1024U;
constexpr std::size_t kMaximumConfiguredAttributes = 4096U;

bool fail_stream(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool add_counter(
    std::uint64_t* value,
    std::uint64_t increment,
    std::string* error,
    std::string_view label) {
    if (*value > std::numeric_limits<std::uint64_t>::max() - increment) {
        return fail_stream(error, std::string(label) + " counter overflow");
    }
    *value += increment;
    return true;
}

bool validate_config(
    const HtmlTokenizerDataStreamV1Config& config,
    std::string* error) {
    if (config.maximum_input_bytes == 0U ||
        config.maximum_input_bytes > kMaximumConfiguredInputBytes) {
        return fail_stream(
            error,
            "HTML Data stream input bound is outside supported range");
    }
    if (config.maximum_token_bytes == 0U ||
        config.maximum_token_bytes > kMaximumConfiguredTokenBytes) {
        return fail_stream(
            error,
            "HTML Data stream token bound is outside supported range");
    }
    if (config.maximum_attributes == 0U ||
        config.maximum_attributes > kMaximumConfiguredAttributes) {
        return fail_stream(
            error,
            "HTML Data stream attribute bound is outside supported range");
    }
    return true;
}

struct SourcePosition {
    std::uint64_t line{1U};
    std::uint64_t column{1U};
};

bool advance_position(
    SourcePosition* position,
    std::string_view consumed,
    std::string* error) {
    for (std::size_t index = 0U; index < consumed.size();) {
        const char character = consumed[index];
        if (character == '\n') {
            if (position->line == std::numeric_limits<std::uint64_t>::max()) {
                return fail_stream(error, "HTML Data stream source line overflow");
            }
            ++position->line;
            position->column = 1U;
            ++index;
            continue;
        }
        std::size_t scalar_bytes = 1U;
        if (static_cast<unsigned char>(character) >= 0x80U) {
            scalar_bytes = detail::html_tokenizer_utf8_scalar_bytes_v1(consumed, index);
            if (scalar_bytes == 0U) {
                return fail_stream(
                    error,
                    "HTML Data stream source-position input contains invalid UTF-8 scalar encoding");
            }
        }
        if (position->column == std::numeric_limits<std::uint64_t>::max()) {
            return fail_stream(error, "HTML Data stream source column overflow");
        }
        ++position->column;
        index += scalar_bytes;
    }
    return true;
}

class OffsetSink final : public HtmlTokenizerV1Sink {
public:
    OffsetSink(
        HtmlTokenizerV1Sink* downstream,
        SourcePosition base)
        : downstream_(downstream), base_(base) {}

    bool on_token(const HtmlTokenizerV1Token& token, std::string* error) override {
        return downstream_->on_token(token, error);
    }

    bool on_parse_error(
        const HtmlTokenizerV1ParseError& local,
        std::string* error) override {
        HtmlTokenizerV1ParseError global = local;
        if (local.line == 0U || local.column == 0U) {
            if (error != nullptr) {
                *error = "HTML Data stream received invalid local parse-error coordinates";
            }
            return false;
        }
        if (local.line == 1U) {
            if (base_.column >
                std::numeric_limits<std::uint64_t>::max() - (local.column - 1U)) {
                if (error != nullptr) {
                    *error = "HTML Data stream parse-error column overflow";
                }
                return false;
            }
            global.line = base_.line;
            global.column = base_.column + local.column - 1U;
        } else {
            if (base_.line >
                std::numeric_limits<std::uint64_t>::max() - (local.line - 1U)) {
                if (error != nullptr) {
                    *error = "HTML Data stream parse-error line overflow";
                }
                return false;
            }
            global.line = base_.line + local.line - 1U;
            global.column = local.column;
        }
        return downstream_->on_parse_error(global, error);
    }

private:
    HtmlTokenizerV1Sink* downstream_{nullptr};
    SourcePosition base_{};
};

std::size_t find_next_markup_open(
    std::string_view input,
    std::size_t start) noexcept {
    bool in_tag = false;
    char quote = '\0';

    for (std::size_t index = start; index < input.size(); ++index) {
        const char character = input[index];
        if (!in_tag) {
            if (character != '<') {
                continue;
            }
            if (index + 1U < input.size() && input[index + 1U] == '!') {
                return index;
            }
            in_tag = true;
            continue;
        }

        if (quote != '\0') {
            if (character == quote) {
                quote = '\0';
            }
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = character;
            continue;
        }
        if (character == '>') {
            in_tag = false;
        }
    }
    return std::string_view::npos;
}

bool aggregate_data_stats(
    HtmlTokenizerDataStreamV1Stats* aggregate,
    const HtmlTokenizerDataTagsV1Stats& part,
    std::string* error) {
    return add_counter(&aggregate->tokens_emitted, part.tokens_emitted, error, "HTML Data stream token") &&
        add_counter(&aggregate->character_tokens_emitted, part.character_tokens_emitted, error, "HTML Data stream character-token") &&
        add_counter(&aggregate->character_bytes_emitted, part.character_bytes_emitted, error, "HTML Data stream character-byte") &&
        add_counter(&aggregate->start_tags_emitted, part.start_tags_emitted, error, "HTML Data stream start-tag") &&
        add_counter(&aggregate->end_tags_emitted, part.end_tags_emitted, error, "HTML Data stream end-tag") &&
        add_counter(&aggregate->comment_tokens_emitted, part.comment_tokens_emitted, error, "HTML Data stream comment-token") &&
        add_counter(&aggregate->attributes_emitted, part.attributes_emitted, error, "HTML Data stream attribute") &&
        add_counter(&aggregate->parse_errors_emitted, part.parse_errors_emitted, error, "HTML Data stream parse-error");
}

bool aggregate_markup_stats(
    HtmlTokenizerDataStreamV1Stats* aggregate,
    const HtmlTokenizerMarkupDeclarationsV1Stats& part,
    std::string* error) {
    return add_counter(&aggregate->tokens_emitted, part.tokens_emitted, error, "HTML Data stream token") &&
        add_counter(&aggregate->comment_tokens_emitted, part.comment_tokens_emitted, error, "HTML Data stream comment-token") &&
        add_counter(&aggregate->doctype_tokens_emitted, part.doctype_tokens_emitted, error, "HTML Data stream DOCTYPE-token") &&
        add_counter(&aggregate->parse_errors_emitted, part.parse_errors_emitted, error, "HTML Data stream parse-error");
}

class DataStreamTokenizer final {
public:
    DataStreamTokenizer(
        std::string_view input,
        HtmlTokenizerDataStreamV1Config config,
        HtmlTokenizerV1Sink* sink,
        HtmlTokenizerDataStreamV1Stats* stats,
        std::string* error)
        : input_(input), config_(config), sink_(sink), stats_(stats), error_(error) {}

    bool run() {
        std::size_t cursor = 0U;
        while (cursor < input_.size()) {
            const std::size_t markup = find_next_markup_open(input_, cursor);
            const std::size_t segment_end =
                markup == std::string_view::npos ? input_.size() : markup;

            if (segment_end > cursor) {
                if (!consume_data_segment(cursor, segment_end)) {
                    return false;
                }
                cursor = segment_end;
            }
            if (markup == std::string_view::npos) {
                return true;
            }
            if (cursor != markup) {
                return fail_stream(
                    error_,
                    "HTML Data stream scanner/delegation cursor diverged");
            }

            const std::string_view declaration_input = input_.substr(markup);
            std::size_t relative_next = 0U;
            HtmlTokenizerMarkupDeclarationsV1Stats markup_stats;
            OffsetSink offset_sink(sink_, source_position_);
            const bool markup_success = consume_html_markup_declaration_v1(
                declaration_input,
                0U,
                HtmlTokenizerMarkupDeclarationsV1Config{config_.maximum_token_bytes},
                &offset_sink,
                &markup_stats,
                &relative_next,
                error_);
            if (!aggregate_markup_stats(stats_, markup_stats, error_)) {
                return false;
            }
            if (!markup_success) {
                return false;
            }
            if (relative_next == 0U || relative_next > declaration_input.size()) {
                return fail_stream(
                    error_,
                    "HTML Data stream markup declaration made no bounded progress");
            }
            if (!advance_position(
                    &source_position_,
                    declaration_input.substr(0U, relative_next),
                    error_)) {
                return false;
            }
            if (!add_counter(
                    &stats_->markup_declarations_consumed,
                    1U,
                    error_,
                    "HTML Data stream markup declaration")) {
                return false;
            }
            cursor += relative_next;
        }
        return true;
    }

private:
    bool consume_data_segment(std::size_t begin, std::size_t end) {
        const std::string_view segment = input_.substr(begin, end - begin);
        HtmlTokenizerDataTagsV1Stats data_stats;
        OffsetSink offset_sink(sink_, source_position_);
        const bool data_success = tokenize_html_data_tags_v1(
            segment,
            HtmlTokenizerDataTagsV1Config{
                config_.maximum_input_bytes,
                config_.maximum_token_bytes,
                config_.maximum_attributes},
            &offset_sink,
            &data_stats,
            error_);
        if (!aggregate_data_stats(stats_, data_stats, error_)) {
            return false;
        }
        if (!data_success) {
            return false;
        }
        if (!advance_position(&source_position_, segment, error_)) {
            return false;
        }
        return add_counter(
            &stats_->data_segments_consumed,
            1U,
            error_,
            "HTML Data stream segment");
    }

    std::string_view input_;
    HtmlTokenizerDataStreamV1Config config_{};
    HtmlTokenizerV1Sink* sink_{nullptr};
    HtmlTokenizerDataStreamV1Stats* stats_{nullptr};
    std::string* error_{nullptr};
    SourcePosition source_position_{};
};

} // namespace

bool tokenize_html_data_stream_v1(
    std::string_view input,
    HtmlTokenizerDataStreamV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerDataStreamV1Stats* stats,
    std::string* error) {
    if (sink == nullptr || error == nullptr) {
        return false;
    }
    error->clear();

    HtmlTokenizerDataStreamV1Stats local_stats{};
    local_stats.input_bytes = static_cast<std::uint64_t>(input.size());
    if (stats != nullptr) {
        *stats = local_stats;
    }
    if (!validate_config(config, error)) {
        return false;
    }
    if (input.size() > config.maximum_input_bytes) {
        return fail_stream(error, "HTML Data stream input exceeds bounded byte limit");
    }

    bool success = false;
    try {
        DataStreamTokenizer tokenizer(input, config, sink, &local_stats, error);
        success = tokenizer.run();
    } catch (const std::bad_alloc&) {
        success = fail_stream(
            error,
            "HTML Data stream allocation failed within bounded v1 composition");
    }

    if (stats != nullptr) {
        *stats = local_stats;
    }
    return success;
}

} // namespace zevryon::massivedoc
