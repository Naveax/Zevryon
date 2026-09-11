#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[1] / "tests/html_tokenizer_script_data_v1_tests.cpp"
text = path.read_text(encoding="utf-8")
replacements = {
    'std::string("<!--x\\0-->", 10U)': 'std::string("<!--x\\0-->", 9U)',
    'std::string("<!--x-\\0-->", 11U)': 'std::string("<!--x-\\0-->", 10U)',
    'std::string("<!--\\0-->", 9U)': 'std::string("<!--\\0-->", 8U)',
    'std::string("<!--<script>\\0</script>-->", 26U)': 'std::string("<!--<script>\\0</script>-->", 25U)',
    'std::string("<!--<script>x-\\0</script>-->", 28U)': 'std::string("<!--<script>x-\\0</script>-->", 27U)',
    'std::string("<!--<script>--\\0</script>-->", 29U)': 'std::string("<!--<script>--\\0</script>-->", 27U)',
}
for old, new in replacements.items():
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected one test-length anchor for {old!r}, found {count}")
    text = text.replace(old, new, 1)
path.write_text(text, encoding="utf-8")
print("corrected Script-data NUL focused fixture byte lengths")
