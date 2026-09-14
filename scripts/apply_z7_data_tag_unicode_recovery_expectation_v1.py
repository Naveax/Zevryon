#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
path = ROOT / "tests/html_tokenizer_data_tag_recovery_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
old = '''    {\n        CollectingSink sink;\n        const std::string input("<\\xC3\\xA9>", 4U);\n        std::string error;\n        if (!require(\n                !tokenize_html_data_tags_v1(input, {}, &sink, nullptr, &error),\n                "tag-open non-ASCII remains fail closed") ||\n            !require(\n                error.find("non-ASCII preprocessing/location authority") !=\n                    std::string::npos,\n                "tag-open non-ASCII failure remains explicit") ||\n            !require(sink.tokens.empty() && sink.errors.empty(),\n                     "tag-open non-ASCII publishes no recovery events")) {\n            return false;\n        }\n    }\n'''
new = '''    {\n        CollectingSink sink;\n        HtmlTokenizerDataTagsV1Stats stats;\n        const std::string input("<\\xC3\\xA9>", 4U);\n        std::string error;\n        if (!require(\n                tokenize_html_data_tags_v1(input, {}, &sink, &stats, &error),\n                std::string("tag-open non-ASCII recovery: ") + error) ||\n            !require(sink.tokens.size() == 1U &&\n                         sink.tokens[0].kind == HtmlTokenizerV1TokenKind::Character &&\n                         sink.tokens[0].data == input,\n                     "tag-open non-ASCII reconsumes through Data") ||\n            !require(sink.errors.size() == 1U &&\n                         sink.errors[0].code == "invalid-first-character-of-tag-name" &&\n                         sink.errors[0].line == 1U && sink.errors[0].column == 2U,\n                     "tag-open non-ASCII exact recovery error") ||\n            !require(stats.tokens_emitted == 1U &&\n                         stats.character_tokens_emitted == 1U &&\n                         stats.parse_errors_emitted == 1U,\n                     "tag-open non-ASCII recovery stats")) {\n            return false;\n        }\n    }\n'''
count = text.count(old)
if count != 1:
    raise SystemExit(f"tag-open Unicode recovery expectation: expected one anchor, found {count}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
print("aligned Data-tag Unicode recovery expectation")
