#!/usr/bin/env python3
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import sys
from typing import Mapping

from m7_collection_admission import (
    CollectionAdmissionInvalid as CollectionAdmissionV2Invalid,
    admit_collection as admit_collection_v2,
)
from m7_json_artifact_snapshot import (
    JsonArtifactSnapshotInvalid,
    read_json_object_snapshot,
)
from m7_physical_browser_full_set import (
    PHYSICAL_BROWSER_FULL_SET_AUTHORITY,
    PhysicalBrowserFullSetInvalid,
    attach_physical_host_evidence,
    validate_physical_browser_full_set,
)


ADMISSION_SCHEMA = "zevryon.competitor.collection-admission.v3"
ADMISSION_AUTHORITY = "m7-physical-stage-and-atomic-artifact-binding-v3"


class CollectionAdmissionInvalid(ValueError):
    pass


def _mapping(value: object, field: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise CollectionAdmissionInvalid(f"{field} must be an object")
    return value


def _browser_stage_receipts(browser_report: Mapping[str, object]) -> dict[str, object]:
    try:
        validate_physical_browser_full_set(browser_report)
        host_before = _mapping(browser_report.get("host_before"), "browser host_before")
        host_after = _mapping(browser_report.get("host_after"), "browser host_after")
        recomputed = attach_physical_host_evidence(
            browser_report,
            host_before=host_before,
            host_after=host_after,
        )
    except (PhysicalBrowserFullSetInvalid, TypeError, ValueError) as exc:
        raise CollectionAdmissionInvalid(
            f"physical browser full-set evidence invalid: {exc}"
        ) from exc

    physical = _mapping(
        recomputed.get("physical_host_evidence"),
        "browser physical_host_evidence",
    )
    return {
        "stage_authority": PHYSICAL_BROWSER_FULL_SET_AUTHORITY,
        "before": copy.deepcopy(physical.get("before")),
        "after": copy.deepcopy(physical.get("after")),
        "physical_host_gate_passed": True,
    }


def admit_collection(
    preflight: Mapping[str, object],
    browser_report: Mapping[str, object],
    zevryon_virtualized: Mapping[str, object],
    zevryon_native_dom: Mapping[str, object],
) -> dict[str, object]:
    browser_stage = _browser_stage_receipts(browser_report)
    try:
        base = admit_collection_v2(
            preflight,
            browser_report,
            zevryon_virtualized,
            zevryon_native_dom,
        )
    except CollectionAdmissionV2Invalid as exc:
        raise CollectionAdmissionInvalid(f"v2 collection admission rejected inputs: {exc}") from exc

    physical = _mapping(base.get("physical_host_evidence"), "physical_host_evidence")
    upgraded_physical = copy.deepcopy(dict(physical))
    upgraded_physical["browser_full_set"] = browser_stage

    return {
        **dict(base),
        "schema": ADMISSION_SCHEMA,
        "admission_authority": ADMISSION_AUTHORITY,
        "physical_host_evidence": upgraded_physical,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Create M7 collection admission v3 from one atomic byte snapshot of each raw "
            "artifact. Browser evidence must use the canonical physical full-set wrapper "
            "and preserve certified before/after M0 machine/thermal receipts."
        )
    )
    parser.add_argument("--preflight", type=Path, required=True)
    parser.add_argument("--browser-report", type=Path, required=True)
    parser.add_argument("--zevryon-virtualized", type=Path, required=True)
    parser.add_argument("--zevryon-native-dom", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    inputs = {
        "preflight": (args.preflight, "runtime preflight"),
        "browser_report": (args.browser_report, "physical browser full-set"),
        "zevryon_virtualized": (args.zevryon_virtualized, "Zevryon virtualized evidence"),
        "zevryon_native_dom": (args.zevryon_native_dom, "Zevryon native-DOM evidence"),
    }
    try:
        snapshots = {
            name: read_json_object_snapshot(path, label=label)
            for name, (path, label) in inputs.items()
        }
        admission = admit_collection(
            snapshots["preflight"].value,
            snapshots["browser_report"].value,
            snapshots["zevryon_virtualized"].value,
            snapshots["zevryon_native_dom"].value,
        )
        admission["input_artifacts"] = {
            name: {
                "path": str(snapshot.path),
                "sha256": snapshot.sha256,
                "byte_count": snapshot.byte_count,
            }
            for name, snapshot in snapshots.items()
        }
    except (
        CollectionAdmissionInvalid,
        JsonArtifactSnapshotInvalid,
        OSError,
    ) as exc:
        print(f"M7 collection admission v3 rejected: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(admission, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if admission["leadership_eligible"] is True else 2


if __name__ == "__main__":
    raise SystemExit(main())
