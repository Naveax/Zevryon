#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
path = ROOT / "src/html_tokenizer_data_tags_v1.cpp"
text = path.read_text(encoding="utf-8")
old = '''            if (character == '/' &&
                *cursor + 1U < input_.size() &&
                input_[*cursor + 1U] == '>') {
                break;
            }
'''
count = text.count(old)
if count != 1:
    raise SystemExit(f"unquoted slash data: expected one anchor, found {count}")
path.write_text(text.replace(old, '', 1), encoding="utf-8")
print("applied unquoted slash token diagnostic")
