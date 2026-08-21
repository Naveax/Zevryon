#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from zevryon_platform.competitor_adapter import (  # noqa: E402
    adapter_request_from_mapping,
    invoke_adapter,
)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run one M7 competitor adapter using the canonical stdin/stdout protocol"
    )
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=900.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("adapter command is required after --")

    raw = json.loads(args.request.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError("adapter request file must contain a JSON object")
    request = adapter_request_from_mapping(raw)
    result = invoke_adapter(command, request, args.timeout_seconds)

    rendered = json.dumps(result.to_dict(), indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if result.run.succeeded else 2


if __name__ == "__main__":
    raise SystemExit(main())
