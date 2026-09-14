#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
path = ROOT / 'scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py'
text = path.read_text(encoding='utf-8')

old = '''    # Ordinary Data NUL, the complete pinned DOCTYPE-NUL family, and
    # normal HTML-comment NUL are admitted. Tag/bogus-comment NUL remains fail closed.
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
            input_text[:9].lower() == "<!doctype" or
            input_text.startswith("<!--") or
            (input_text.startswith("<") and not input_text.startswith("<!"))
        ):
            pass
        else:
            return "input-preprocessing-nul"
'''
new = '''    # All pinned NUL families in admitted initial states now have production
    # semantics: state-specific text handling, ordinary Data, tags, comments,
    # DOCTYPE, and markup-declaration bogus comments. Keep this classifier as
    # an explicit guard so future initial-state expansion cannot silently inherit
    # NUL authority without its own proof.
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
            input_text[:9].lower() == "<!doctype" or
            input_text.startswith("<!--") or
            (input_text.startswith("<") and not input_text.startswith("<!")) or
            (
                input_text.startswith("<!") and
                not input_text.startswith("<!--") and
                input_text[:9].lower() != "<!doctype"
            )
        ):
            pass
        else:
            return "input-preprocessing-nul"
'''
if text.count(old) != 1:
    raise SystemExit(f'NUL classifier anchor drift: {text.count(old)}')
text = text.replace(old, new, 1)

anchor = '''    require(
        classify_pre_execution("tokenizer/test3.test", "Data state", "<a\\x00>", "", []) is None,
        "tag-family Data NUL is classified admitted",
    )
'''
insert = anchor + '''    require(
        classify_pre_execution("tokenizer/test3.test", "Data state", "<!\\x00", "", []) is None,
        "markup-declaration bogus-comment NUL is classified admitted",
    )
    require(
        classify_pre_execution("tokenizer/test4.test", "Data state", "<!doc>\\x00", "", []) is None,
        "post-bogus-comment Data NUL is classified admitted",
    )
'''
if text.count(anchor) != 1:
    raise SystemExit(f'NUL self-test anchor drift: {text.count(anchor)}')
text = text.replace(anchor, insert, 1)
path.write_text(text, encoding='utf-8')
print('applied residual NUL census gate cleanup')
