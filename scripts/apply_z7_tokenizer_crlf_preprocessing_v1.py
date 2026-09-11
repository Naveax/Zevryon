#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


cpp_path = ROOT / "src/html_tokenizer_token_stream_v1.cpp"
cpp = cpp_path.read_text(encoding="utf-8")

helper_anchor = '''bool fail_tokenizer(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool ascii_alpha(char value) noexcept {
'''
helper_replacement = '''bool fail_tokenizer(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool preprocess_html_input_newlines_v1(
    std::string_view input,
    std::string* storage,
    std::string_view* normalized,
    std::string* error) {
    *normalized = input;
    if (input.find('\\r') == std::string_view::npos) {
        return true;
    }

    try {
        storage->clear();
        storage->reserve(input.size());
        for (std::size_t index = 0U; index < input.size(); ++index) {
            const char character = input[index];
            if (character != '\\r') {
                storage->push_back(character);
                continue;
            }

            storage->push_back('\\n');
            if (index + 1U < input.size() && input[index + 1U] == '\\n') {
                ++index;
            }
        }
    } catch (const std::bad_alloc&) {
        return fail_tokenizer(
            error,
            "HTML tokenizer CR/CRLF preprocessing allocation failed within bounded input");
    }

    *normalized = std::string_view(*storage);
    return true;
}

bool ascii_alpha(char value) noexcept {
'''
cpp = replace_once(cpp, helper_anchor, helper_replacement, "newline helper")

entry_anchor = '''    if (input.size() > config.maximum_input_bytes) {
        return fail_tokenizer(
            error,
            "HTML tokenizer input exceeds bounded byte limit");
    }

    if (initial_state == HtmlTokenizerV1InitialState::ScriptData) {
'''
entry_replacement = '''    if (input.size() > config.maximum_input_bytes) {
        return fail_tokenizer(
            error,
            "HTML tokenizer input exceeds bounded byte limit");
    }

    std::string preprocessed_input_storage;
    std::string_view tokenizer_input = input;
    if (!preprocess_html_input_newlines_v1(
            input,
            &preprocessed_input_storage,
            &tokenizer_input,
            error)) {
        if (stats != nullptr) {
            *stats = local_stats;
        }
        return false;
    }

    if (initial_state == HtmlTokenizerV1InitialState::ScriptData) {
'''
cpp = replace_once(cpp, entry_anchor, entry_replacement, "entry preprocessing")
cpp = replace_once(
    cpp,
    '''        const bool success = run_script_data_canonical(
            input,
''',
    '''        const bool success = run_script_data_canonical(
            tokenizer_input,
''',
    "Script-data dispatch",
)
cpp = replace_once(
    cpp,
    '''        const bool success = run_data_canonical(
            input,
''',
    '''        const bool success = run_data_canonical(
            tokenizer_input,
''',
    "Data dispatch",
)
cpp = replace_once(
    cpp,
    '''        const bool success = run_cdata_canonical(
            input,
''',
    '''        const bool success = run_cdata_canonical(
            tokenizer_input,
''',
    "CDATA dispatch",
)
cpp = replace_once(
    cpp,
    '''        Tokenizer tokenizer(
            input,
            active_state,
''',
    '''        Tokenizer tokenizer(
            tokenizer_input,
            active_state,
''',
    "text-state dispatch",
)
cpp_path.write_text(cpp, encoding="utf-8")


tests_path = ROOT / "tests/html_tokenizer_token_stream_v1_tests.cpp"
tests = tests_path.read_text(encoding="utf-8")
new_test = r'''bool test_crlf_input_preprocessing() {
    return run_case(
               "Data CR normalization",
               HtmlTokenizerV1InitialState::Data,
               "",
               "a\rb",
               {character("a\nb")},
               {}) &&
        run_case(
               "PLAINTEXT CRLF and CR normalization",
               HtmlTokenizerV1InitialState::Plaintext,
               "plaintext",
               "a\r\nb\rc",
               {character("a\nb\nc")},
               {}) &&
        run_case(
               "RCDATA CRLF normalization",
               HtmlTokenizerV1InitialState::Rcdata,
               "xmp",
               "a\r\nb",
               {character("a\nb")},
               {}) &&
        run_case(
               "RAWTEXT CR normalization",
               HtmlTokenizerV1InitialState::Rawtext,
               "xmp",
               "a\rb",
               {character("a\nb")},
               {}) &&
        run_case(
               "Script-data CRLF normalization",
               HtmlTokenizerV1InitialState::ScriptData,
               "script",
               "a\r\nb",
               {character("a\nb")},
               {}) &&
        run_case(
               "CDATA CRLF normalization",
               HtmlTokenizerV1InitialState::CdataSection,
               "",
               "a\r\nb]]>",
               {character("a\nb")},
               {});
}

'''
tests = replace_once(
    tests,
    "bool test_nul_preprocessing_gap_fails_closed() {\n",
    new_test + "bool test_nul_preprocessing_gap_fails_closed() {\n",
    "focused CR test insertion",
)
tests = replace_once(
    tests,
    '''        !test_character_token_bound_fails_closed() ||
        !test_nul_preprocessing_gap_fails_closed() ||
''',
    '''        !test_character_token_bound_fails_closed() ||
        !test_crlf_input_preprocessing() ||
        !test_nul_preprocessing_gap_fails_closed() ||
''',
    "focused CR main registration",
)
tests_path.write_text(tests, encoding="utf-8")


census_path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
census = census_path.read_text(encoding="utf-8")
census = replace_once(
    census,
    '''    if "\\r" in input_text:
        return "input-preprocessing-cr"
    return None
''',
    '''    return None
''',
    "CR census admission",
)
census_path.write_text(census, encoding="utf-8")

print("applied bounded CR/CRLF preprocessing diagnostic patch")
