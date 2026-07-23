from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest


MODULE_PATH = (
    Path(__file__).resolve().parents[1]
    / "tools"
    / "generate_unicode_bidi_mirroring.py"
)
SPEC = importlib.util.spec_from_file_location("bidi_mirroring_generator", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def unicode_data_line(codepoint: str, name: str, mirrored: str) -> str:
    fields = [
        codepoint,
        name,
        "Ps",
        "0",
        "ON",
        "",
        "",
        "",
        "",
        mirrored,
        "",
        "",
        "",
        "",
        "",
    ]
    assert len(fields) == 15
    return ";".join(fields)


def test_normative_property_and_informative_mapping_are_separate(tmp_path: Path) -> None:
    unicode_data = tmp_path / "UnicodeData.txt"
    unicode_data.write_text(
        "\n".join(
            [
                unicode_data_line("0028", "LEFT PARENTHESIS", "Y"),
                unicode_data_line("0029", "RIGHT PARENTHESIS", "Y"),
                unicode_data_line("221B", "CUBE ROOT", "Y"),
                unicode_data_line("FD3E", "ORNATE LEFT PARENTHESIS", "N"),
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    bidi_mirroring = tmp_path / "BidiMirroring.txt"
    bidi_mirroring.write_text(
        "0028; 0029 # LEFT PARENTHESIS\n"
        "0029; 0028 # RIGHT PARENTHESIS\n",
        encoding="utf-8",
    )

    mirrored = MODULE.parse_unicode_data(unicode_data)
    mappings = MODULE.parse_bidi_mirroring(bidi_mirroring)

    assert mirrored == [0x28, 0x29, 0x221B]
    assert [mapping.codepoint for mapping in mappings] == [0x28, 0x29]
    assert 0x221B not in {mapping.codepoint for mapping in mappings}
    assert 0xFD3E not in mirrored


def test_ranges_and_best_fit_are_deterministic(tmp_path: Path) -> None:
    unicode_data = tmp_path / "UnicodeData.txt"
    unicode_data.write_text(
        "\n".join(
            [
                unicode_data_line("1000", "<TEST RANGE, First>", "Y"),
                unicode_data_line("1002", "<TEST RANGE, Last>", "Y"),
                unicode_data_line("2000", "SINGLE MIRROR", "Y"),
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    bidi_mirroring = tmp_path / "BidiMirroring.txt"
    bidi_mirroring.write_text(
        "1000; 1002 # [BEST FIT] TEST\n"
        "1002; 1000 # TEST\n",
        encoding="utf-8",
    )

    mirrored = MODULE.parse_unicode_data(unicode_data)
    mappings = MODULE.parse_bidi_mirroring(bidi_mirroring)

    assert mirrored == [0x1000, 0x1001, 0x1002, 0x2000]
    assert MODULE.contiguous_ranges(mirrored) == [(0x1000, 0x1002), (0x2000, 0x2000)]
    assert mappings[0].best_fit is True
    assert mappings[1].best_fit is False
    assert MODULE.normalized_fingerprint(mirrored, mappings) == MODULE.normalized_fingerprint(
        list(mirrored), list(mappings)
    )


def test_mapping_source_without_normative_property_is_detectable(tmp_path: Path) -> None:
    unicode_data = tmp_path / "UnicodeData.txt"
    unicode_data.write_text(
        unicode_data_line("0028", "LEFT PARENTHESIS", "N") + "\n",
        encoding="utf-8",
    )
    bidi_mirroring = tmp_path / "BidiMirroring.txt"
    bidi_mirroring.write_text("0028; 0029\n", encoding="utf-8")

    mirrored = MODULE.parse_unicode_data(unicode_data)
    mappings = MODULE.parse_bidi_mirroring(bidi_mirroring)
    missing_sources = [
        mapping.codepoint for mapping in mappings if mapping.codepoint not in set(mirrored)
    ]
    assert missing_sources == [0x28]


def test_malformed_unicode_data_is_rejected(tmp_path: Path) -> None:
    unicode_data = tmp_path / "UnicodeData.txt"
    unicode_data.write_text("0028;TOO;SHORT\n", encoding="utf-8")
    with pytest.raises(ValueError, match="expected 15 fields"):
        MODULE.parse_unicode_data(unicode_data)
