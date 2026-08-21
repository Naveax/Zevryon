from __future__ import annotations

import argparse
from pathlib import Path

from zevryon_platform.benchmark_metadata import (
    capture_benchmark_metadata,
    physical_certification_checks,
)


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture Zevryon physical benchmark metadata")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--require-physical", action="store_true")
    args = parser.parse_args()

    metadata = capture_benchmark_metadata()
    checks = physical_certification_checks(metadata)
    if args.require_physical and not checks["physical_metadata_complete"]:
        missing = ", ".join(name for name, passed in checks.items() if not passed)
        raise SystemExit(f"physical benchmark metadata incomplete: {missing}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    temporary.write_text(metadata.to_json() + "\n", encoding="utf-8")
    temporary.replace(args.output)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
