#!/usr/bin/env python3
from __future__ import annotations

from typing import Any

import m8_final_evidence_binder_impl as implementation
from m8_bundle_common import EvidenceInvalid
from m8_profile_observation_gate import EvidenceInvalid as ProfileEvidenceInvalid


_original_gate = implementation.gate
_original_validate_profile = implementation.validate_profile


def _strict_gate(condition: bool, message: str) -> None:
    if not condition and message.startswith("required raw artifact is missing:"):
        raise EvidenceInvalid(message)
    _original_gate(condition, message)


def _strict_validate_profile(raw: bytes, plan: dict[str, Any]) -> dict[str, Any]:
    try:
        return _original_validate_profile(raw, plan)
    except ProfileEvidenceInvalid as exc:
        raise EvidenceInvalid(f"profile evidence invalid: {exc}") from exc


def main() -> int:
    # Keep the large raw-evidence recomputation implementation byte-for-byte stable.
    # This entrypoint owns only final evidence/gate classification semantics.
    implementation.gate = _strict_gate
    implementation.validate_profile = _strict_validate_profile
    return implementation.main()


if __name__ == "__main__":
    raise SystemExit(main())
