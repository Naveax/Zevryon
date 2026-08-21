#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from zevryon_platform.competitor_lab import evaluate_campaign_payload  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Evaluate a Zevryon M7 competitor-laboratory campaign"
    )
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--require-leadership", action="store_true")
    args = parser.parse_args()

    payload = json.loads(args.input.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("competitor campaign input must be a JSON object")
    result = evaluate_campaign_payload(payload)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")

    if args.require_leadership and not bool(
        result["leadership"]["leadership_claim_allowed"]
    ):
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
