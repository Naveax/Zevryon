#!/usr/bin/env python3
from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[1]
full = ROOT / "scripts/.bogus_nul_old_helper.py"
text = full.read_text(encoding="utf-8")
marker = 'path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"'
if marker not in text:
    raise SystemExit("bogus helper census marker missing")
source_helper = ROOT / "scripts/.bogus_nul_source_helper.py"
source_helper.write_text(text.split(marker, 1)[0] + '\nprint("applied bogus-comment NUL source/test candidate")\n', encoding="utf-8")
runpy.run_path(str(source_helper), run_name="__main__")
source_helper.unlink(missing_ok=True)
full.unlink(missing_ok=True)

path = ROOT / "scripts/z7_html5lib_tokenizer_full_corpus_census_v1.py"
text = path.read_text(encoding="utf-8")
old = '''    # Ordinary Data NUL and the complete pinned DOCTYPE-NUL family are
    # admitted. Comment/tag NUL remains fail closed.
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
            "<" not in input_text or input_text[:9].lower() == "<!doctype"
        ):
            pass
        else:
            return "input-preprocessing-nul"
'''
new = '''    # Diagnostic admission: ordinary Data, complete pinned DOCTYPE-NUL,
    # and markup-declaration bogus-comment NUL. Normal comments/tag-family NUL remain fail closed.
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
    raise SystemExit(f"bogus classifier anchor count={text.count(old)}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
print("applied bogus-comment NUL after DOCTYPE diagnostic patch")
