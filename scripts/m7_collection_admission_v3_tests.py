#!/usr/bin/env python3
from __future__ import annotations

import copy

from m7_collection_admission_tests import fixtures
from m7_collection_admission_v3 import (
    ADMISSION_AUTHORITY,
    ADMISSION_SCHEMA,
    CollectionAdmissionInvalid,
    admit_collection,
)
from m7_physical_browser_full_set import attach_physical_host_evidence


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except CollectionAdmissionInvalid:
        return
    raise AssertionError(message)


def physical_fixtures():
    preflight, browser, virtual, native = fixtures()
    host = copy.deepcopy(browser["host"])
    browser = attach_physical_host_evidence(
        browser,
        host_before=copy.deepcopy(host),
        host_after=copy.deepcopy(host),
    )
    return preflight, browser, virtual, native


def main() -> int:
    preflight, browser, virtual, native = physical_fixtures()
    admitted = admit_collection(preflight, browser, virtual, native)
    require(admitted["schema"] == ADMISSION_SCHEMA, "v3 admission schema drifted")
    require(
        admitted["admission_authority"] == ADMISSION_AUTHORITY,
        "v3 admission authority drifted",
    )
    browser_stage = admitted["physical_host_evidence"]["browser_full_set"]
    require(
        browser_stage["physical_host_gate_passed"] is True,
        "browser physical stage gate did not pass",
    )
    require(
        browser_stage["before"]["checks"]["physical_metadata_complete"] is True,
        "browser pre-stage physical receipt was lost",
    )
    require(
        browser_stage["after"]["checks"]["physical_metadata_complete"] is True,
        "browser post-stage physical receipt was lost",
    )

    no_wrapper = copy.deepcopy(browser)
    no_wrapper.pop("physical_browser_full_set_authority")
    require_invalid(
        lambda: admit_collection(preflight, no_wrapper, virtual, native),
        "browser report without physical full-set authority was accepted",
    )

    no_post_thermal = copy.deepcopy(browser)
    no_post_thermal["host_after"]["benchmark_machine_metadata"]["thermal"] = {
        "state": "unknown",
        "source": "unavailable",
        "readings_c": [],
    }
    require_invalid(
        lambda: admit_collection(preflight, no_post_thermal, virtual, native),
        "browser full-set without post-stage thermal observation was accepted",
    )

    forged_embedded = copy.deepcopy(browser)
    forged_embedded["physical_host_evidence"]["after"]["cpu_model"] = "forged"
    require_invalid(
        lambda: admit_collection(preflight, forged_embedded, virtual, native),
        "forged browser post-stage receipt was accepted",
    )

    post_machine_drift = copy.deepcopy(browser)
    post_machine_drift["host_after"]["cpu_model"] = "Different CPU"
    post_machine_drift["host_after"]["benchmark_machine_metadata"]["cpu_model"] = "Different CPU"
    require_invalid(
        lambda: admit_collection(preflight, post_machine_drift, virtual, native),
        "browser post-stage machine identity drift was accepted",
    )

    print("M7 collection admission v3 tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
