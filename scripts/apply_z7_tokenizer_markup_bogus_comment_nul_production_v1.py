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
''',
    "comment emitter split",
)
text = replace_once(
    text,
    '''    bool consume_bogus_comment() {
        const std::size_t data_begin = offset_ + 2U;
        if (!emit_parse_error(data_begin, "incorrectly-opened-comment")) {
            return false;
        }
        std::size_t cursor = data_begin;
        while (cursor < input_.size() && input_[cursor] != '>') {
            if (input_[cursor] == '\\0') {
                return fail_markup(
                    error_,
                    "HTML markup declaration input preprocessing/NUL replacement is not implemented");
            }
            if (!ascii_byte(input_[cursor])) {
                return fail_markup(
                    error_,
                    "HTML markup declaration non-ASCII bogus-comment authority is not implemented");
            }
            ++cursor;
        }
        const std::string_view data = input_.substr(data_begin, cursor - data_begin);
        if (!emit_comment(data)) {
            return false;
        }
        *next_offset_ = cursor < input_.size() ? cursor + 1U : cursor;
        return true;
    }
''',
    '''    bool consume_bogus_comment() {
        const std::size_t data_begin = offset_ + 2U;
        if (!emit_parse_error(data_begin, "incorrectly-opened-comment")) {
            return false;
        }
        std::size_t cursor = data_begin;
        std::string data;
        data.reserve(std::min<std::size_t>(
            input_.size() - data_begin,
            config_.maximum_token_bytes));
        constexpr std::string_view replacement = "\\xEF\\xBF\\xBD";
        while (cursor < input_.size() && input_[cursor] != '>') {
            const char character = input_[cursor];
            if (character == '\\0') {
                if (!emit_parse_error(cursor, "unexpected-null-character")) {
                    return false;
                }
                if (data.size() > config_.maximum_token_bytes ||
                    replacement.size() > config_.maximum_token_bytes - data.size()) {
                    return fail_markup(
                        error_,
                        "HTML markup declaration comment token exceeds bounded byte limit");
                }
                data.append(replacement.data(), replacement.size());
                ++cursor;
                continue;
            }
            if (!ascii_byte(character)) {
                return fail_markup(
                    error_,
                    "HTML markup declaration non-ASCII bogus-comment authority is not implemented");
            }
            if (data.size() >= config_.maximum_token_bytes) {
                return fail_markup(
                    error_,
                    "HTML markup declaration comment token exceeds bounded byte limit");
            }
            data.push_back(character);
            ++cursor;
        }
        if (!emit_comment_owned(std::move(data))) {
            return false;
        }
        *next_offset_ = cursor < input_.size() ? cursor + 1U : cursor;
        return true;
    }
''',
    "bogus comment NUL semantics",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "tests/html_tokenizer_markup_declarations_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
insert_before = '''bool test_fail_closed_boundaries() {
'''
new_test = r'''bool test_bogus_comment_nul_semantics() {
    std::string replacement("\xEF\xBF\xBD", 3U);
    {
        std::string input = "<!";
        input.push_back('\0');
        if (!run_comment_case(
                "bogus comment NUL",
                input,
                replacement,
                {
                    ExpectedError{"incorrectly-opened-comment", 1U, 3U},
                    ExpectedError{"unexpected-null-character", 1U, 3U},
                })) {
            return false;
        }
    }
    {
        std::string input = "<! ";
        input.push_back('\0');
        std::string expected = " ";
        expected += replacement;
        if (!run_comment_case(
                "bogus comment spaced NUL",
                input,
                expected,
                {
                    ExpectedError{"incorrectly-opened-comment", 1U, 3U},
                    ExpectedError{"unexpected-null-character", 1U, 4U},
                })) {
            return false;
        }
    }
    return true;
}

'''
text = replace_once(text, insert_before, new_test + insert_before, "bogus comment NUL focused tests")
text = replace_once(
    text,
    '''        !test_pinned_test1_comments() ||
        !test_nonzero_offset_preserves_global_location() ||
        !test_fail_closed_boundaries()) {
''',
    '''        !test_pinned_test1_comments() ||
        !test_nonzero_offset_preserves_global_location() ||
        !test_bogus_comment_nul_semantics() ||
        !test_fail_closed_boundaries()) {
''',
    "bogus comment NUL test registration",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    '''    # CDATA preserves raw NUL. PLAINTEXT/RCDATA/RAWTEXT and Script data
    # admit their state-specific behavior. Data admits only ordinary character
    # data here; any '<' keeps markup/comment/tag/DOCTYPE NUL fail closed.
    if "\\x00" in input_text:
        if state_name in {
            "PLAINTEXT state",
            "RCDATA state",
            "RAWTEXT state",
            "Script data state",
            "CDATA section state",
        }:
            pass
        elif state_name == "Data state" and "<" not in input_text:
            pass
        else:
            return "input-preprocessing-nul"
''',
    '''    # Ordinary Data NUL plus the proven markup-declaration bogus-comment
    # NUL family are admitted. Normal comments, DOCTYPE and tag-family NUL stay
    # fail closed in this production slice.
    if "\\x00" in input_text:
        if state_name in {
            "PLAINTEXT state",
            "RCDATA state",
            "RAWTEXT state",
            "Script data state",
            "CDATA section state",
        }:
            pass
        elif state_name == "Data state" and (
            "<" not in input_text or
            (
                input_text.startswith("<!") and
                not input_text.startswith("<!--") and
                input_text[:9].lower() != "<!doctype"
            )
        ):
            pass
        else:
            return "input-preprocessing-nul"
''',
    "markup bogus-comment NUL production classifier",
)
path.write_text(text, encoding="utf-8")

path = ROOT / "docs/Z7_HTML_TOKENIZER_MARKUP_DECLARATIONS_V1.md"
text = path.read_text(encoding="utf-8")
text = text.replace(
    "- standard and bounded comment / bogus-comment handling already admitted by the component;",
    "- standard and bounded comment / bogus-comment handling, including proven U+0000 replacement in markup-declaration bogus comments;",
    1,
)
text = text.replace(
    "- raw NUL replacement/input-stream preprocessing;",
    "- raw NUL replacement/input-stream preprocessing outside the separately proven ordinary Data and markup-declaration bogus-comment slices;",
    1,
)
path.write_text(text, encoding="utf-8")

print("applied markup bogus-comment NUL production patch")
