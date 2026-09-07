#!/usr/bin/env python3
from __future__ import annotations

"""Run the admitted profile-case probe smoke against the UTF-8-safe Titan implementation."""

from pathlib import Path
import importlib.util
import sys

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL = ROOT / "scripts" / "m8_profile_case_probe_tests.py"
SPEC = importlib.util.spec_from_file_location("m8_profile_case_probe_tests_original", ORIGINAL)
assert SPEC is not None and SPEC.loader is not None
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)
module.TITAN_SCRIPT = ROOT / "scripts" / "m8_titan_fixture_utf8.py"

if __name__ == "__main__":
    raise SystemExit(module.main())
