#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from z7_wpt_tree_runner_v1 import DEFAULT_MANIFEST, RunnerError, run


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Require exact conformance for the complete configured frozen Z7 WPT tree authority"
    )
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args()
    try:
        report = run(args.probe.resolve(), args.manifest.resolve())
        if report.get("tree_builder_conformance_claim") is not True:
            raise RunnerError(
                "configured WPT tree authority is not fully conformant: "
                f"passed={report.get('passed')} failed={report.get('failed')} "
                f"unsupported={report.get('unsupported')} executions={report.get('executions')}"
            )
    except RunnerError as exc:
        print(f"Z7 WPT tree conformance gate failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
