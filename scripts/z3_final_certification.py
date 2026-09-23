from __future__ import annotations

from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    scope_tests = sorted(
        str(path.relative_to(ROOT))
        for path in (ROOT / "tests").glob("test_z3_*scope.py")
    )
    required = [
        "tests/test_z3_gate_closure.py",
        "tests/test_z3_authority_aggregation.py",
        "tests/test_zenith_program_contract.py",
        *scope_tests,
    ]
    if len(scope_tests) < 5:
        raise RuntimeError(
            f"refusing incomplete Z3 certification discovery: {len(scope_tests)} scope tests"
        )
    return pytest.main(["-q", *required])


if __name__ == "__main__":
    raise SystemExit(main())
