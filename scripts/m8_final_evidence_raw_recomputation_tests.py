#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import sys

SOURCE_ROOT = Path(__file__).resolve().parents[1]
if str(SOURCE_ROOT) not in sys.path:
    sys.path.insert(0, str(SOURCE_ROOT))

import m8_final_evidence_binder_tests as legacy  # noqa: E402

legacy.BINDER = Path(__file__).resolve().with_name("m8_final_evidence_binder_impl.py")

if __name__ == "__main__":
    raise SystemExit(legacy.main())
