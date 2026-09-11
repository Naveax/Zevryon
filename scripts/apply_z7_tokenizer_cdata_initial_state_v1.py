#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected exactly one anchor in {path}, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


hpp = ROOT / "src/html_tokenizer_token_stream_v1.hpp"
replace_once(
    hpp,
    "    Rawtext,\n    ScriptData,\n};\n",
    "    Rawtext,\n    ScriptData,\n    CdataSection,\n};\n",
)
replace_once(
    hpp,
    "// tokenizer-conformance surface. V1 accepts Data, PLAINTEXT, RCDATA, RAWTEXT\n"
    "// and Script data as explicit initial states. Data delegates to the admitted\n",
    "// tokenizer-conformance surface. V1 accepts Data, PLAINTEXT, RCDATA, RAWTEXT,\n"
    "// Script data and CDATA section as explicit initial states. Data delegates to the admitted\n",
)
replace_once(
    hpp,
    "// This remains intentionally narrower than complete WHATWG tokenization.\n"
    "// CDATA, full input-stream preprocessing/non-ASCII raw-input location authority\n",
    "// This remains intentionally narrower than complete WHATWG tokenization.\n"
    "// Full input-stream preprocessing/non-ASCII raw-input location authority\n",
)

cpp = ROOT / "src/html_tokenizer_token_stream_v1.cpp"
insert_anchor = """bool account_script_stats(
    const HtmlTokenizerScriptDataV1Stats& source,
"""
helpers = r'''bool emit_canonical_parse_error(
    std::string_view full_input,
    std::size_t offset,
    std::string code,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerV1Stats* stats,
    std::string* error) {
    HtmlTokenizerV1ParseError parse_error =
        position_error(full_input, offset, std::move(code));
    if (!sink->on_parse_error(parse_error, error)) {
        if (error->empty()) {
            *error = "HTML tokenizer sink rejected parse error";
        }
        return false;
    }
    return increment_counter(
        &stats->parse_errors_emitted,
        error,
        "HTML tokenizer parse-error");
}

bool emit_canonical_character(
    std::string_view data,
    const HtmlTokenizerV1Config& config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerV1Stats* stats,
    std::string* error) {
    if (data.empty()) {
        return true;
    }
    if (data.size() > config.maximum_token_bytes) {
        return fail_tokenizer(
            error,
            "HTML tokenizer CDATA character token exceeds bounded byte limit");
    }
    HtmlTokenizerV1Token token;
    token.kind = HtmlTokenizerV1TokenKind::Character;
    token.data.assign(data.data(), data.size());
    if (!sink->on_token(token, error)) {
        if (error->empty()) {
            *error = "HTML tokenizer sink rejected CDATA character token";
        }
        return false;
    }
    return increment_counter(
               &stats->tokens_emitted,
               error,
               "HTML tokenizer token") &&
        increment_counter(
               &stats->character_tokens_emitted,
               error,
               "HTML tokenizer character-token") &&
        add_counter(
               &stats->character_bytes_emitted,
               static_cast<std::uint64_t>(data.size()),
               error,
               "HTML tokenizer character-byte");
}

class CdataContinuationSink final : public HtmlTokenizerV1Sink {
public:
    CdataContinuationSink(
        HtmlTokenizerV1Sink* downstream,
        std::string prefix,
        std::size_t maximum_token_bytes,
        std::uint64_t base_line,
        std::uint64_t base_column,
        HtmlTokenizerV1Stats* stats)
        : downstream_(downstream),
          prefix_(std::move(prefix)),
          maximum_token_bytes_(maximum_token_bytes),
          base_line_(base_line),
          base_column_(base_column),
          stats_(stats) {}

    bool on_token(const HtmlTokenizerV1Token& token, std::string* error) override {
        if (!prefix_.empty() && token.kind == HtmlTokenizerV1TokenKind::Character) {
            if (prefix_.size() > maximum_token_bytes_ ||
                token.data.size() > maximum_token_bytes_ - prefix_.size()) {
                return fail_tokenizer(
                    error,
                    "HTML tokenizer CDATA/Data coalesced character token exceeds bounded byte limit");
            }
            HtmlTokenizerV1Token merged = token;
            merged.data.insert(0U, prefix_);
            prefix_.clear();
            return emit_counted_token(merged, error);
        }
        if (!flush_prefix(error)) {
            return false;
        }
        return emit_counted_token(token, error);
    }

    bool on_parse_error(
        const HtmlTokenizerV1ParseError& parse_error,
        std::string* error) override {
        HtmlTokenizerV1ParseError translated = parse_error;
        if (parse_error.line == 1U) {
            translated.line = base_line_;
            translated.column = base_column_ + parse_error.column - 1U;
        } else {
            translated.line = base_line_ + parse_error.line - 1U;
            translated.column = parse_error.column;
        }
        if (!downstream_->on_parse_error(translated, error)) {
            if (error->empty()) {
                *error = "HTML tokenizer sink rejected translated Data parse error";
            }
            return false;
        }
        return increment_counter(
            &stats_->parse_errors_emitted,
            error,
            "HTML tokenizer parse-error");
    }

    bool finish(std::string* error) {
        return flush_prefix(error);
    }

private:
    bool emit_counted_token(const HtmlTokenizerV1Token& token, std::string* error) {
        if (!downstream_->on_token(token, error)) {
            if (error->empty()) {
                *error = "HTML tokenizer sink rejected CDATA/Data continuation token";
            }
            return false;
        }
        if (!increment_counter(
                &stats_->tokens_emitted,
                error,
                "HTML tokenizer token")) {
            return false;
        }
        if (token.kind == HtmlTokenizerV1TokenKind::Character) {
            return increment_counter(
                       &stats_->character_tokens_emitted,
                       error,
                       "HTML tokenizer character-token") &&
                add_counter(
                       &stats_->character_bytes_emitted,
                       static_cast<std::uint64_t>(token.data.size()),
                       error,
                       "HTML tokenizer character-byte");
        }
        if (token.kind == HtmlTokenizerV1TokenKind::EndTag) {
            return increment_counter(
                &stats_->end_tags_emitted,
                error,
                "HTML tokenizer end-tag");
        }
        return true;
    }

    bool flush_prefix(std::string* error) {
        if (prefix_.empty()) {
            return true;
        }
        HtmlTokenizerV1Token token;
        token.kind = HtmlTokenizerV1TokenKind::Character;
        token.data = std::move(prefix_);
        prefix_.clear();
        return emit_counted_token(token, error);
    }

    HtmlTokenizerV1Sink* downstream_{nullptr};
    std::string prefix_;
    std::size_t maximum_token_bytes_{0U};
    std::uint64_t base_line_{1U};
    std::uint64_t base_column_{1U};
    HtmlTokenizerV1Stats* stats_{nullptr};
};

bool run_cdata_canonical(
    std::string_view input,
    HtmlTokenizerV1Config config,
    HtmlTokenizerV1Sink* sink,
    HtmlTokenizerV1Stats* stats,
    std::string* error) {
    const std::size_t marker = input.find("]]>");
    const std::size_t prefix_end =
        marker == std::string_view::npos ? input.size() : marker;

    for (std::size_t index = 0U; index < prefix_end; ++index) {
        if (input_control_parse_error(input[index]) &&
            !emit_canonical_parse_error(
                input,
                index,
                "control-character-in-input-stream",
                sink,
                stats,
                error)) {
            return false;
        }
    }

    if (marker == std::string_view::npos) {
        if (!emit_canonical_character(input, config, sink, stats, error)) {
            return false;
        }
        return emit_canonical_parse_error(
            input,
            input.size(),
            "eof-in-cdata",
            sink,
            stats,
            error);
    }

    const std::size_t data_offset = marker + 3U;
    if (data_offset == input.size()) {
        return emit_canonical_character(
            input.substr(0U, marker),
            config,
            sink,
            stats,
            error);
    }

    const HtmlTokenizerV1ParseError base =
        position_error(input, data_offset, "");
    CdataContinuationSink translated_sink(
        sink,
        std::string(input.substr(0U, marker)),
        config.maximum_token_bytes,
        base.line,
        base.column,
        stats);

    HtmlTokenizerDataStreamV1Config data_config{};
    data_config.maximum_input_bytes = config.maximum_input_bytes;
    data_config.maximum_token_bytes = config.maximum_token_bytes;
    data_config.maximum_attributes = 256U;
    HtmlTokenizerDataStreamV1Stats ignored_data_stats{};
    const bool success = tokenize_html_data_stream_v1(
        input.substr(data_offset),
        data_config,
        &translated_sink,
        &ignored_data_stats,
        error);
    if (!success) {
        return false;
    }
    return translated_sink.finish(error);
}

'''
replace_once(cpp, insert_anchor, helpers + insert_anchor)

replace_once(
    cpp,
    """    if (initial_state == HtmlTokenizerV1InitialState::Data) {
        const bool success = run_data_canonical(
            input,
            config,
            sink,
            &local_stats,
            error);
        if (stats != nullptr) {
            *stats = local_stats;
        }
        return success;
    }

    ActiveState active_state = ActiveState::Plaintext;
""",
    """    if (initial_state == HtmlTokenizerV1InitialState::Data) {
        const bool success = run_data_canonical(
            input,
            config,
            sink,
            &local_stats,
            error);
        if (stats != nullptr) {
            *stats = local_stats;
        }
        return success;
    }

    if (initial_state == HtmlTokenizerV1InitialState::CdataSection) {
        const bool success = run_cdata_canonical(
            input,
            config,
            sink,
            &local_stats,
            error);
        if (stats != nullptr) {
            *stats = local_stats;
        }
        return success;
    }

    ActiveState active_state = ActiveState::Plaintext;
""",
)
replace_once(
    cpp,
    """    case HtmlTokenizerV1InitialState::ScriptData:
        return fail_tokenizer(
            error,
            "HTML tokenizer Script-data dispatch invariant failed");
    case HtmlTokenizerV1InitialState::Data:
""",
    """    case HtmlTokenizerV1InitialState::ScriptData:
        return fail_tokenizer(
            error,
            "HTML tokenizer Script-data dispatch invariant failed");
    case HtmlTokenizerV1InitialState::CdataSection:
        return fail_tokenizer(
            error,
            "HTML tokenizer CDATA-section dispatch invariant failed");
    case HtmlTokenizerV1InitialState::Data:
""",
)

probe = ROOT / "tests/html_tokenizer_token_stream_v1_probe.cpp"
replace_once(
    probe,
    """    if (value == "SCRIPT_DATA") {
        *state = HtmlTokenizerV1InitialState::ScriptData;
        return true;
    }
    return false;
""",
    """    if (value == "SCRIPT_DATA") {
        *state = HtmlTokenizerV1InitialState::ScriptData;
        return true;
    }
    if (value == "CDATA_SECTION") {
        *state = HtmlTokenizerV1InitialState::CdataSection;
        return true;
    }
    return false;
""",
)
replace_once(
    probe,
    "usage: probe <DATA|PLAINTEXT|RCDATA|RAWTEXT|SCRIPT_DATA> <last-tag-hex> <input-hex>",
    "usage: probe <DATA|PLAINTEXT|RCDATA|RAWTEXT|SCRIPT_DATA|CDATA_SECTION> <last-tag-hex> <input-hex>",
)

census = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
replace_once(
    census,
    """    "RAWTEXT state": "RAWTEXT",
    "Script data state": "SCRIPT_DATA",
}
""",
    """    "RAWTEXT state": "RAWTEXT",
    "Script data state": "SCRIPT_DATA",
    "CDATA section state": "CDATA_SECTION",
}
""",
)
replace_once(
    census,
    """    if "\\x00" in input_text:
        return "input-preprocessing-nul"
""",
    """    # The html5lib CDATA initial-state authority intentionally preserves raw
    # NUL as Character data. Do not route that state through the generic
    # Data/text-state NUL preprocessing debt bucket.
    if "\\x00" in input_text and state_name != "CDATA section state":
        return "input-preprocessing-nul"
""",
)
replace_once(
    census,
    """    require(
        classify_pre_execution("tokenizer/test2.test", "CDATA section state", "x", "", []) == "initial-state:CDATA section state",
        "CDATA unsupported classification",
    )
""",
    """    require(
        classify_pre_execution("tokenizer/test2.test", "CDATA section state", "x", "", []) is None,
        "CDATA admitted classification",
    )
    require(
        classify_pre_execution("tokenizer/domjs.test", "CDATA section state", "\\x00]]>", "", []) is None,
        "CDATA raw NUL bypasses generic preprocessing bucket",
    )
""",
)

tests = ROOT / "tests/html_tokenizer_token_stream_v1_tests.cpp"
replace_once(
    tests,
    """    case HtmlTokenizerV1InitialState::ScriptData:
        return "SCRIPT_DATA";
    }
""",
    """    case HtmlTokenizerV1InitialState::ScriptData:
        return "SCRIPT_DATA";
    case HtmlTokenizerV1InitialState::CdataSection:
        return "CDATA_SECTION";
    }
""",
)
test_anchor = """bool test_character_token_bound_fails_closed() {
"""
test_fn = r'''bool test_cdata_section_initial_state() {
    if (!run_case(
            "CDATA empty EOF",
            HtmlTokenizerV1InitialState::CdataSection,
            "",
            "",
            {},
            {ExpectedError{"eof-in-cdata", 1U, 1U}}) ||
        !run_case(
            "CDATA literal content EOF",
            HtmlTokenizerV1InitialState::CdataSection,
            "",
            "foo",
            {character("foo")},
            {ExpectedError{"eof-in-cdata", 1U, 4U}}) ||
        !run_case(
            "CDATA control then EOF",
            HtmlTokenizerV1InitialState::CdataSection,
            "",
            std::string(1U, '\x0B'),
            {character(std::string(1U, '\x0B'))},
            {
                ExpectedError{"control-character-in-input-stream", 1U, 1U},
                ExpectedError{"eof-in-cdata", 1U, 2U},
            }) ||
        !run_case(
            "CDATA marker at EOF",
            HtmlTokenizerV1InitialState::CdataSection,
            "",
            "foo&#32;]]>",
            {character("foo&#32;")},
            {}) ||
        !run_case(
            "CDATA continuation coalesces into Data character output",
            HtmlTokenizerV1InitialState::CdataSection,
            "",
            "foo&#32;]]>&#32;",
            {character("foo&#32; ")},
            {}) ||
        !run_case(
            "CDATA extra bracket",
            HtmlTokenizerV1InitialState::CdataSection,
            "",
            "foo]]]>",
            {character("foo]")},
            {}) ||
        !run_case(
            "CDATA preserves raw NUL",
            HtmlTokenizerV1InitialState::CdataSection,
            "",
            std::string("\0]]>", 4U),
            {character(std::string(1U, '\0'))},
            {})) {
        return false;
    }

    {
        CollectingSink sink;
        HtmlTokenizerV1Config config;
        config.maximum_token_bytes = 3U;
        std::string error;
        if (!require(
                !tokenize_html_token_stream_v1(
                    "abcd",
                    HtmlTokenizerV1InitialState::CdataSection,
                    "",
                    config,
                    &sink,
                    nullptr,
                    &error),
                "CDATA character token hard cap rejects oversized token") ||
            !require(
                error.find("CDATA character token exceeds bounded byte limit") != std::string::npos,
                "CDATA hard-cap failure is explicit") ||
            !require(sink.tokens.empty(), "CDATA hard-cap failure publishes no partial token")) {
            return false;
        }
    }
    {
        CollectingSink sink;
        HtmlTokenizerV1Config config;
        config.maximum_token_bytes = 3U;
        std::string error;
        if (!require(
                !tokenize_html_token_stream_v1(
                    "ab]]>cd",
                    HtmlTokenizerV1InitialState::CdataSection,
                    "",
                    config,
                    &sink,
                    nullptr,
                    &error),
                "CDATA/Data merged token hard cap rejects oversized token") ||
            !require(
                error.find("CDATA/Data coalesced character token exceeds bounded byte limit") != std::string::npos,
                "CDATA/Data merged hard-cap failure is explicit") ||
            !require(sink.tokens.empty(), "CDATA/Data merged hard-cap failure publishes no partial token")) {
            return false;
        }
    }
    return true;
}

'''
replace_once(tests, test_anchor, test_fn + test_anchor)
replace_once(
    tests,
    """    if (!test_pinned_content_model_flag_semantics() ||
        !test_character_token_bound_fails_closed() ||
""",
    """    if (!test_pinned_content_model_flag_semantics() ||
        !test_cdata_section_initial_state() ||
        !test_character_token_bound_fails_closed() ||
""",
)

doc = ROOT / "docs/Z7_HTML_TOKENIZER_CDATA_SECTION_V1.md"
doc.write_text(
    """# Z7 bounded CDATA section initial state v1\n\n"
    "This slice admits the pinned html5lib `CDATA section state` initial-state surface at the canonical token-stream boundary.\n\n"
    "## Admitted behavior\n\n"
    "- bytes before the first `]]>` are emitted as Character data;\n"
    "- an extra closing bracket before `]]>` remains Character data;\n"
    "- `]]>` itself emits no token and transitions to the already-admitted canonical Data stream;\n"
    "- adjacent Character data is coalesced across the CDATA-to-Data transition;\n"
    "- EOF before `]]>` emits `eof-in-cdata` at the exact EOF location while preserving accumulated Character data;\n"
    "- admitted ASCII input controls retain `control-character-in-input-stream` diagnostics;\n"
    "- raw NUL in this specific CDATA authority remains a literal NUL Character, matching the pinned html5lib fixture rather than the generic Data/text-state NUL debt bucket;\n"
    "- Character output remains bounded by `maximum_token_bytes`, including coalescing across the Data transition.\n\n"
    "## Nonclaims\n\n"
    "This does not admit general raw-NUL replacement, CR normalization, malformed UTF-8/non-scalar input, XML infoset coercion, or complete tokenizer conformance.\n",
    encoding="utf-8",
)

print("applied Z7 CDATA initial-state v1")
