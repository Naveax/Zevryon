#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if text.count(old) != 1:
        raise SystemExit(f"expected exactly one anchor in {path}, found {text.count(old)}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


hpp = ROOT / "src/html_tokenizer_data_tags_v1.hpp"
replace_once(
    hpp,
    "    std::uint64_t start_tags_emitted{0U};\n"
    "    std::uint64_t end_tags_emitted{0U};\n"
    "    std::uint64_t attributes_emitted{0U};\n",
    "    std::uint64_t start_tags_emitted{0U};\n"
    "    std::uint64_t end_tags_emitted{0U};\n"
    "    std::uint64_t comment_tokens_emitted{0U};\n"
    "    std::uint64_t attributes_emitted{0U};\n",
)
replace_once(
    hpp,
    "// tag/attribute names or raw non-ASCII attribute values, nor the broader malformed-\n"
    "// tag and bogus-comment recovery states. Those surfaces fail closed until\n",
    "// tag/attribute names or raw non-ASCII attribute values, nor the remaining malformed-\n"
    "// tag recovery states. Those surfaces fail closed until\n",
)

data_stream = ROOT / "src/html_tokenizer_data_stream_v1.cpp"
replace_once(
    data_stream,
    "        add_counter(&aggregate->end_tags_emitted, part.end_tags_emitted, error, \"HTML Data stream end-tag\") &&\n"
    "        add_counter(&aggregate->attributes_emitted, part.attributes_emitted, error, \"HTML Data stream attribute\") &&\n",
    "        add_counter(&aggregate->end_tags_emitted, part.end_tags_emitted, error, \"HTML Data stream end-tag\") &&\n"
    "        add_counter(&aggregate->comment_tokens_emitted, part.comment_tokens_emitted, error, \"HTML Data stream comment-token\") &&\n"
    "        add_counter(&aggregate->attributes_emitted, part.attributes_emitted, error, \"HTML Data stream attribute\") &&\n",
)

cpp = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
insert_anchor = """    bool recover_eof_before_tag_name(
        std::size_t* cursor,
        std::string_view literal) {
"""
helpers = r'''    bool emit_comment(std::string data) {
        if (!flush_character()) {
            return false;
        }
        HtmlTokenizerV1Token token;
        token.kind = HtmlTokenizerV1TokenKind::Comment;
        token.data = std::move(data);
        if (!sink_->on_token(token, error_)) {
            if (error_->empty()) {
                *error_ = "HTML Data-tag tokenizer sink rejected comment token";
            }
            return false;
        }
        return increment_counter(
                   &stats_->tokens_emitted,
                   error_,
                   "HTML Data-tag tokenizer token") &&
            increment_counter(
                &stats_->comment_tokens_emitted,
                error_,
                "HTML Data-tag tokenizer comment-token");
    }

    bool consume_bogus_comment(
        std::size_t* cursor,
        std::size_t data_begin,
        std::size_t error_offset,
        std::string error_code) {
        if (cursor == nullptr || data_begin > input_.size()) {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer bogus-comment dispatch invariant failed");
        }
        if (!emit_parse_error(error_offset, std::move(error_code))) {
            return false;
        }

        std::string data;
        data.reserve(std::min<std::size_t>(
            input_.size() - data_begin,
            config_.maximum_token_bytes));
        std::size_t scan = data_begin;
        while (scan < input_.size() && input_[scan] != '>') {
            const char character = input_[scan];
            if (character == '\0') {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer input preprocessing/NUL replacement is not implemented");
            }
            if (!ascii_byte(character)) {
                const std::size_t scalar_bytes =
                    detail::html_tokenizer_utf8_scalar_bytes_v1(input_, scan);
                if (scalar_bytes == 0U) {
                    return fail_data_tokenizer(
                        error_,
                        "HTML Data-tag tokenizer bogus comment contains invalid UTF-8 scalar encoding");
                }
                if (!append_bounded_bytes(
                        &data,
                        input_.substr(scan, scalar_bytes),
                        "HTML Data-tag tokenizer bogus comment token")) {
                    return false;
                }
                scan += scalar_bytes;
                continue;
            }
            if (ascii_control_parse_error(character) &&
                !emit_parse_error(scan, "control-character-in-input-stream")) {
                return false;
            }
            if (!append_bounded_bytes(
                    &data,
                    input_.substr(scan, 1U),
                    "HTML Data-tag tokenizer bogus comment token")) {
                return false;
            }
            ++scan;
        }

        if (!emit_comment(std::move(data))) {
            return false;
        }
        *cursor = scan < input_.size() ? scan + 1U : scan;
        return true;
    }

'''
replace_once(cpp, insert_anchor, helpers + insert_anchor)

replace_once(
    cpp,
    """        if (input_[probe] == '?') {
            return fail_data_tokenizer(
                error_,
                "HTML Data-tag tokenizer bogus-comment recovery is outside admitted v1 subset");
        }
""",
    """        if (input_[probe] == '?') {
            return consume_bogus_comment(
                cursor,
                probe,
                probe,
                "unexpected-question-mark-instead-of-tag-name");
        }
""",
)
replace_once(
    cpp,
    """            if (end_tag) {
                return fail_data_tokenizer(
                    error_,
                    "HTML Data-tag tokenizer bogus-comment end-tag recovery is outside admitted v1 subset");
            }
""",
    """            if (end_tag) {
                return consume_bogus_comment(
                    cursor,
                    probe,
                    probe,
                    "invalid-first-character-of-tag-name");
            }
""",
)

tests = ROOT / "tests/html_tokenizer_data_tags_v1_tests.cpp"
test_anchor = """bool test_admitted_references_and_fail_closed_boundaries() {
"""
test_function = r'''bool test_bogus_comment_recovery() {
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Stats stats;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1(
                    "<?namespace>", {}, &sink, &stats, &error),
                std::string("processing-instruction bogus comment: ") + error) ||
            !require(sink.tokens.size() == 1U, "processing-instruction comment token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[0].data == "?namespace",
                "processing-instruction comment payload") ||
            !require(sink.errors.size() == 1U, "processing-instruction error count") ||
            !require(
                sink.errors[0].code == "unexpected-question-mark-instead-of-tag-name" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 2U,
                "processing-instruction error position") ||
            !require(stats.tokens_emitted == 1U && stats.comment_tokens_emitted == 1U,
                     "processing-instruction comment stats")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<?foo-->", {}, &sink, nullptr, &error),
                std::string("bogus comment close: ") + error) ||
            !require(sink.tokens.size() == 1U, "bogus comment close token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[0].data == "?foo--",
                "bogus comment stops at first greater-than") ||
            !require(sink.errors.size() == 1U, "bogus comment close error count")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("</1>", {}, &sink, nullptr, &error),
                std::string("invalid end-tag bogus comment: ") + error) ||
            !require(sink.tokens.size() == 1U, "invalid end-tag comment token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[0].data == "1",
                "invalid end-tag comment payload") ||
            !require(sink.errors.size() == 1U, "invalid end-tag error count") ||
            !require(
                sink.errors[0].code == "invalid-first-character-of-tag-name" &&
                    sink.errors[0].line == 1U && sink.errors[0].column == 3U,
                "invalid end-tag error position")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("a<?b>c", {}, &sink, nullptr, &error),
                std::string("bogus comment event ordering: ") + error) ||
            !require(sink.tokens.size() == 3U, "bogus comment event ordering token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[0].data == "a",
                "character data flushes before bogus comment") ||
            !require(
                sink.tokens[1].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[1].data == "?b",
                "bogus comment is emitted between character runs") ||
            !require(
                sink.tokens[2].kind == HtmlTokenizerV1TokenKind::Character &&
                    sink.tokens[2].data == "c",
                "character data resumes after bogus comment")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        if (!require(
                tokenize_html_data_tags_v1("<?foo", {}, &sink, nullptr, &error),
                std::string("bogus comment EOF: ") + error) ||
            !require(sink.tokens.size() == 1U, "bogus comment EOF token count") ||
            !require(
                sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Comment &&
                    sink.tokens[0].data == "?foo",
                "bogus comment EOF payload") ||
            !require(sink.errors.size() == 1U, "bogus comment EOF has only entry parse error")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        std::string error;
        const std::string input("<?\v>", 4U);
        if (!require(
                tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),
                std::string("bogus comment control input: ") + error) ||
            !require(sink.tokens.size() == 1U, "bogus comment control token count") ||
            !require(sink.tokens[0].data == std::string("?\v", 2U),
                     "bogus comment retains admitted control byte") ||
            !require(sink.errors.size() == 2U, "bogus comment control error count") ||
            !require(
                sink.errors[0].code == "unexpected-question-mark-instead-of-tag-name" &&
                    sink.errors[0].column == 2U,
                "bogus comment entry error precedes input control error") ||
            !require(
                sink.errors[1].code == "control-character-in-input-stream" &&
                    sink.errors[1].column == 3U,
                "bogus comment control error position")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerDataTagsV1Config config;
        config.maximum_token_bytes = 3U;
        std::string error;
        if (!require(
                !tokenize_html_data_tags_v1("<?abcd>", config, &sink, nullptr, &error),
                "bogus comment token bound rejects oversized payload") ||
            !require(
                error.find("bogus comment token exceeds bounded byte limit") != std::string::npos,
                "bogus comment token bound failure is explicit") ||
            !require(sink.tokens.empty(), "oversized bogus comment publishes no comment token")) {
            return false;
        }
    }
    return true;
}

'''
replace_once(tests, test_anchor, test_function + test_anchor)
replace_once(
    tests,
    """        !test_tag_eof_recovery() ||
        !test_end_tag_attributes_are_diagnosed_and_dropped() ||
""",
    """        !test_tag_eof_recovery() ||
        !test_bogus_comment_recovery() ||
        !test_end_tag_attributes_are_diagnosed_and_dropped() ||
""",
)

doc = ROOT / "docs/Z7_HTML_TOKENIZER_DATA_TAGS_V1.md"
replace_once(
    doc,
    "- coalesced Character tokens outside tags;\n",
    "- coalesced Character tokens outside tags;\n"
    "- bounded bogus Comment tokens entered from `<?...` tag-open recovery and invalid end-tag-open bytes;\n",
)
replace_once(
    doc,
    "- empty end tag `</>`: `missing-end-tag-name`, no EndTag token;\n",
    "- empty end tag `</>`: `missing-end-tag-name`, no EndTag token;\n"
    "- `<?...` tag-open recovery: `unexpected-question-mark-instead-of-tag-name`, reconsuming `?` into a bounded bogus Comment token;\n"
    "- invalid ASCII end-tag-open recovery such as `</1>`: `invalid-first-character-of-tag-name`, reconsuming the offending byte into a bounded bogus Comment token;\n"
    "- bogus Comment data stops at the first `>` or EOF, preserves admitted UTF-8 scalar bytes, reports admitted input controls and obeys the token byte cap;\n",
)
replace_once(
    doc,
    "- `<?...` bogus-comment recovery;\n"
    "- bogus-comment recovery for invalid end-tag-open bytes other than the admitted empty `</>` case;\n",
    "",
)
replace_once(
    doc,
    "The established Data-tag suite covers normalized tags, attributes, duplicate/missing-whitespace diagnostics, end-tag diagnostics, `<plaintext>` separation and hard caps.\n",
    "The established Data-tag suite covers normalized tags, attributes, duplicate/missing-whitespace diagnostics, end-tag diagnostics, `<plaintext>` separation and hard caps. It also covers bogus-comment entry from `<?...` and invalid end-tag-open bytes, EOF termination, event ordering, control diagnostics and the comment token byte cap.\n",
)

print("applied Z7 Data bogus-comment recovery v1")
