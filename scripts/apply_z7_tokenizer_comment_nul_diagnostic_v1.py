#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


path = ROOT / "src/html_tokenizer_markup_declarations_v1.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    bool emit_comment(std::string_view data) {
        if (data.size() > config_.maximum_token_bytes) {
            return fail_markup(
                error_,
                "HTML markup declaration comment token exceeds bounded byte limit");
        }
        if (!validate_ascii_span(data, error_, "comment")) {
            return false;
        }
        HtmlTokenizerV1Token token;
        token.kind = HtmlTokenizerV1TokenKind::Comment;
        token.data.assign(data.data(), data.size());
        if (!sink_->on_token(token, error_)) {
            if (error_->empty()) {
                *error_ = "HTML markup declaration sink rejected comment token";
            }
            return false;
        }
        return increment_counter(
                   &stats_->tokens_emitted,
                   error_,
                   "HTML markup declaration token") &&
            increment_counter(
                &stats_->comment_tokens_emitted,
                error_,
                "HTML markup declaration comment-token");
    }
''',
    '''    bool emit_comment_owned(std::string data) {
        if (data.size() > config_.maximum_token_bytes) {
            return fail_markup(
                error_,
                "HTML markup declaration comment token exceeds bounded byte limit");
        }
        HtmlTokenizerV1Token token;
        token.kind = HtmlTokenizerV1TokenKind::Comment;
        token.data = std::move(data);
        if (!sink_->on_token(token, error_)) {
            if (error_->empty()) {
                *error_ = "HTML markup declaration sink rejected comment token";
            }
            return false;
        }
        return increment_counter(
                   &stats_->tokens_emitted,
                   error_,
                   "HTML markup declaration token") &&
            increment_counter(
                &stats_->comment_tokens_emitted,
                error_,
                "HTML markup declaration comment-token");
    }

    bool emit_comment(std::string_view data) {
        if (!validate_ascii_span(data, error_, "comment")) {
            return false;
        }
        return emit_comment_owned(std::string(data));
    }

    bool collect_comment_data(
        std::size_t begin,
        std::size_t end,
        std::string* data) {
        if (data == nullptr || begin > end || end > input_.size()) {
            return fail_markup(error_, "HTML markup declaration comment range invariant failed");
        }
        data->clear();
        data->reserve(std::min<std::size_t>(end - begin, config_.maximum_token_bytes));
        for (std::size_t cursor = begin; cursor < end; ++cursor) {
            const char value = input_[cursor];
            if (value == '\\0') {
                if (!emit_parse_error(cursor, "unexpected-null-character")) {
                    return false;
                }
                constexpr std::string_view replacement = "\\xEF\\xBF\\xBD";
                if (data->size() > config_.maximum_token_bytes -
                        std::min(config_.maximum_token_bytes, replacement.size())) {
                    return fail_markup(
                        error_,
                        "HTML markup declaration comment token exceeds bounded byte limit");
                }
                data->append(replacement.data(), replacement.size());
                continue;
            }
            if (!ascii_byte(value)) {
                return fail_markup(
                    error_,
                    "HTML markup declaration non-ASCII comment authority is not implemented");
            }
            if (data->size() >= config_.maximum_token_bytes) {
                return fail_markup(
                    error_,
                    "HTML markup declaration comment token exceeds bounded byte limit");
            }
            data->push_back(value);
        }
        return true;
    }
''',
    "comment emitter split",
)
text = replace_once(
    text,
    '''            const std::string_view data = input_.substr(data_begin, close - data_begin);
            if (!emit_comment(data)) {
                return false;
            }
''',
    '''            std::string data;
            if (!collect_comment_data(data_begin, close, &data) ||
                !emit_comment_owned(std::move(data))) {
                return false;
            }
''',
    "closed comment NUL collection",
)
text = replace_once(
    text,
    '''        const std::string_view data = input_.substr(data_begin, data_end - data_begin);
        if (!emit_parse_error(input_.size(), "eof-in-comment") ||
            !emit_comment(data)) {
            return false;
        }
''',
    '''        std::string data;
        if (!collect_comment_data(data_begin, data_end, &data) ||
            !emit_parse_error(input_.size(), "eof-in-comment") ||
            !emit_comment_owned(std::move(data))) {
            return false;
        }
''',
    "EOF comment NUL collection",
)
path.write_text(text, encoding="utf-8")


path = ROOT / "tests/html_tokenizer_markup_declarations_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    {
        CollectingSink sink;
        const std::string input("<!--a\\0b-->", 10U);
        HtmlTokenizerMarkupDeclarationsV1Stats stats;
        std::size_t next_offset = 0U;
        std::string error;
        if (!require(
                !consume_html_markup_declaration_v1(
                    input, 0U, {}, &sink, &stats, &next_offset, &error),
                "comment NUL remains fail closed") ||
            !require(error.find("preprocessing/NUL replacement") != std::string::npos,
                     "comment NUL failure explicit") ||
            !require(sink.tokens.empty(), "comment NUL publishes no token")) {
            return false;
        }
    }
''',
    '''    {
        CollectingSink sink;
        const std::string input("<!--a\\0b-->", 10U);
        HtmlTokenizerMarkupDeclarationsV1Stats stats;
        std::size_t next_offset = 0U;
        std::string error;
        std::string expected = "a";
        expected.append("\\xEF\\xBF\\xBD", 3U);
        expected.push_back('b');
        if (!require(
                consume_html_markup_declaration_v1(
                    input, 0U, {}, &sink, &stats, &next_offset, &error),
                std::string("comment NUL replacement: ") + error) ||
            !require(next_offset == input.size(), "comment NUL consumes input") ||
            !require(sink.tokens.size() == 1U &&
                         sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Comment &&
                         sink.tokens[0].data == expected,
                     "comment NUL replacement payload") ||
            !require(sink.errors.size() == 1U &&
                         sink.errors[0].code == "unexpected-null-character" &&
                         sink.errors[0].line == 1U && sink.errors[0].column == 6U,
                     "comment NUL diagnostic") ||
            !require(stats.parse_errors_emitted == 1U,
                     "comment NUL parse-error stats")) {
            return false;
        }
    }
''',
    "comment NUL focused regression",
)
path.write_text(text, encoding="utf-8")


path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
text = path.read_text(encoding="utf-8")
old = '''    # CDATA preserves raw NUL. PLAINTEXT/RCDATA/RAWTEXT and Script data
    # now admit their state-specific U+FFFD replacement behavior. Data remains
    # behind the separate NUL authority boundary because its markup/comment/
    # DOCTYPE state family needs a distinct bounded admission slice.
    if "\\x00" in input_text and state_name not in {
        "PLAINTEXT state",
        "RCDATA state",
        "RAWTEXT state",
        "Script data state",
        "CDATA section state",
    }:
        return "input-preprocessing-nul"
'''
new = '''    # Diagnostic-only admission of normal HTML comment NUL. Other Data-state
    # NUL surfaces remain preclassified unsupported.
    if "\\x00" in input_text and state_name not in {
        "PLAINTEXT state",
        "RCDATA state",
        "RAWTEXT state",
        "Script data state",
        "CDATA section state",
    }:
        if state_name == "Data state" and input_text.startswith("<!--"):
            pass
        else:
            return "input-preprocessing-nul"
'''
text = replace_once(text, old, new, "comment NUL diagnostic classifier")
path.write_text(text, encoding="utf-8")

print("applied normal-comment NUL diagnostic patch")
