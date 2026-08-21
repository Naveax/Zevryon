#!/usr/bin/env python3
from __future__ import annotations

import copy
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from zevryon_platform.competitor_workload import parse_canonical_workload  # noqa: E402

SHA = "ab" * 32


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def canonical() -> dict[str, object]:
    return {
        "schema": "zevryon.m7.workload.v1",
        "corpus_sha256": SHA,
        "corpus_logical_bytes": 1024 * 1024,
        "viewport": {
            "width_px": 1440,
            "height_px": 900,
            "overscan_px": 720,
            "max_fragments": 512,
        },
        "operations": [
            {"kind": "open_preindexed"},
            {"kind": "open_streaming"},
            {"kind": "scroll", "samples": 2000, "warmup": 120, "step_px": 18},
            {"kind": "exact_search_warm", "query_utf8": "needle", "trials": 10},
            {
                "kind": "exact_search_cold",
                "query_utf8": "needle",
                "trials": 10,
                "fresh_process_each_trial": True,
            },
            {"kind": "mutation_batch", "count": 1000},
            {"kind": "copy_all", "trials": 5},
        ],
    }


def reject(payload: dict[str, object], fragment: str) -> None:
    try:
        parse_canonical_workload(payload)
    except ValueError as error:
        require(fragment in str(error), f"wrong rejection: {error}")
    else:
        raise AssertionError(f"invalid workload accepted: expected {fragment}")


def main() -> int:
    payload = canonical()
    parsed = parse_canonical_workload(payload)
    require(parsed.to_dict() == payload, "canonical workload changed")
    require(len(parsed.sha256) == 64, "workload hash missing")

    reordered = copy.deepcopy(payload)
    reordered["operations"][2], reordered["operations"][3] = (
        reordered["operations"][3],
        reordered["operations"][2],
    )
    reject(reordered, "canonical M7 order")

    short_scroll = copy.deepcopy(payload)
    short_scroll["operations"][2]["samples"] = 999
    reject(short_scroll, "scroll.samples")

    cold_same_process = copy.deepcopy(payload)
    cold_same_process["operations"][4]["fresh_process_each_trial"] = False
    reject(cold_same_process, "fresh process")

    query_mismatch = copy.deepcopy(payload)
    query_mismatch["operations"][4]["query_utf8"] = "other"
    reject(query_mismatch, "same query")

    extra = copy.deepcopy(payload)
    extra["viewport"]["device_scale_factor"] = 1
    reject(extra, "viewport fields mismatch")

    normalized = copy.deepcopy(payload)
    normalized["operations"][3]["query_utf8"] = " needle "
    normalized["operations"][4]["query_utf8"] = " needle "
    parsed2 = parse_canonical_workload(normalized)
    require(parsed2.search_query_utf8 == " needle ", "query bytes were normalized")

    print("Zevryon M7 canonical workload smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
