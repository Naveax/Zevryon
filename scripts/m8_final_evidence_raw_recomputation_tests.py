#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

import m8_final_evidence_binder_tests as legacy

legacy.BINDER = Path(__file__).resolve().with_name("m8_final_evidence_binder_impl.py")

if __name__ == "__main__":
    raise SystemExit(legacy.main())
