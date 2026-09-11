#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().with_name("apply_z7_tokenizer_cdata_initial_state_v1.py")
source = path.read_text(encoding="utf-8")
old = '    """# Z7 bounded CDATA section initial state v1\\n\\n"\n'
new = '    "# Z7 bounded CDATA section initial state v1\\n\\n"\n'
if source.count(old) != 1:
    raise SystemExit(f"expected one staging syntax anchor, found {source.count(old)}")
source = source.replace(old, new, 1)
exec(compile(source, str(path), "exec"), {"__name__": "__main__", "__file__": str(path)})
