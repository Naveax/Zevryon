#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_benchmark_evidence import host_metadata
from browser_competitor_registry import CANONICAL_KEYS, get_spec
from m7_collection_admission import (
    CollectionAdmissionInvalid,
    admit_collection,
    stable_runtime_identity,
)
from m7_leadership_evaluator_tests import (
    CORPUS_SHA,
    PAYLOAD_BYTES,
    VIRTUAL_SLICE_BYTES,
    browser_report as evaluator_browser_report,
    zevryon_case,
)
from m7_normalized_browser_full_set import COLLECTION_AUTHORITY
from m7_runtime_preflight import run_runtime_preflight


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except CollectionAdmissionInvalid:
        return
    raise AssertionError(message)


def generic_probe(competitor: str) -> dict[str, object]:
    spec = get_spec(competitor)
    if competitor == "servo":
        identity = (
            "servo|binary=/opt/servo|version=Servo test|"
            "webdriver=127.0.0.1:39001|headless=true"
        )
    elif competitor == "ladybird":
        identity = (
            "ladybird|webdriver_binary=/opt/ladybird/WebDriver|sha256="
            + "e" * 64
            + "|webdriver=127.0.0.1:39002|headless=true"
        )
    else:
        identity = f"{competitor}|test-runtime"
    return {
        "competitor": competitor,
        "canonical_name": spec.canonical_name,
        "adapter": spec.adapter,
        "status": "success",
        "launch_ready": True,
        "runtime_identity": identity,
        "reason": None,
    }


def webdriver_probe(competitor: str) -> dict[str, object]:
    return generic_probe(competitor)


def _rebind_identity(target: dict[str, object], host: dict[str, object], fingerprint: str) -> None:
    target["host_platform"] = str(host["platform"])
    target["host_arch"] = str(host["arch"])
    target["system_fingerprint"] = fingerprint
    normalized = target.get("normalized_core_evidence")
    if isinstance(normalized, dict):
        normalized["host_platform"] = str(host["platform"])
        normalized["host_arch"] = str(host["arch"])
        normalized["system_fingerprint"] = fingerprint


def fixtures() -> tuple[dict[str, object], dict[str, object], dict[str, object], dict[str, object]]:
    preflight = run_runtime_preflight(
        playwright_probe=generic_probe,
        webdriver_probe=webdriver_probe,
    )
    host = dict(preflight["host"])
    fingerprint = str(preflight["system_fingerprint"])

    browser = evaluator_browser_report()
    browser["collection_authority"] = COLLECTION_AUTHORITY
    browser["host"] = host
    browser["corpus_sha256"] = CORPUS_SHA
    browser["payload_bytes"] = PAYLOAD_BYTES
    browser["virtual_slice_bytes"] = VIRTUAL_SLICE_BYTES

    preflight_records = {
        str(record["competitor"]): record for record in preflight["records"]
    }
    webdriver_ports = {
        "servo": {"virtualized": 41001, "native-dom": 41002},
        "ladybird": {"virtualized": 42001, "native-dom": 42002},
    }
    for case in browser["browser_cases"]:
        _rebind_identity(case, host, fingerprint)
        competitor = str(case["competitor"])
        if competitor == "servo":
            case["runtime_identity"] = (
                "servo|binary=/opt/servo|version=Servo test|"
                f"webdriver=127.0.0.1:{webdriver_ports['servo'][str(case['mode'])]}|"
                "headless=true"
            )
        elif competitor == "ladybird":
            case["runtime_identity"] = (
                "ladybird|webdriver_binary=/opt/ladybird/WebDriver|sha256="
                + "e" * 64
                + f"|webdriver=127.0.0.1:{webdriver_ports['ladybird'][str(case['mode'])]}|"
                "headless=true"
            )
        else:
            case["runtime_identity"] = str(
                preflight_records[competitor]["runtime_identity"]
            )

    virtual = zevryon_case("virtualized")
    native = zevryon_case("native-dom")
    _rebind_identity(virtual, host, fingerprint)
    _rebind_identity(native, host, fingerprint)
    return preflight, browser, virtual, native


def main() -> int:
    servo_one = (
        "servo|binary=/opt/servo|version=Servo test|"
        "webdriver=127.0.0.1:12345|headless=true"
    )
    servo_two = (
        "servo|binary=/opt/servo|version=Servo test|"
        "webdriver=127.0.0.1:54321|headless=true"
    )
    require(
        stable_runtime_identity("servo", servo_one)
        == stable_runtime_identity("servo", servo_two),
        "ephemeral Servo ports changed stable runtime identity",
    )
    require(
        stable_runtime_identity("chrome", "chrome|test-runtime")
        == "chrome|test-runtime",
        "Playwright stable identity was rewritten",
    )
    require_invalid(
        lambda: stable_runtime_identity("servo", "servo|missing-endpoint"),
        "WebDriver identity without endpoint was accepted",
    )

    preflight, browser, virtual, native = fixtures()
    admitted = admit_collection(preflight, browser, virtual, native)
    require(admitted["leadership_metric_gate_evaluated"] is True, "metric gate not evaluated")
    require(admitted["leadership_eligible"] is True, "valid collection was not eligible")
    require(
        set(admitted["runtime_bindings"]) == set(CANONICAL_KEYS),
        "runtime binding set drifted",
    )
    require(
        all(
            binding["matched"] is True
            for binding in admitted["runtime_bindings"].values()
        ),
        "valid runtime binding did not match",
    )

    runtime_drift = copy.deepcopy(browser)
    chrome = next(
        case
        for case in runtime_drift["browser_cases"]
        if case["competitor"] == "chrome" and case["mode"] == "native-dom"
    )
    chrome["runtime_identity"] = "chrome|different-runtime"
    require_invalid(
        lambda: admit_collection(preflight, runtime_drift, virtual, native),
        "runtime identity drift between preflight and measurement was accepted",
    )

    webdriver_version_drift = copy.deepcopy(browser)
    servo = next(
        case
        for case in webdriver_version_drift["browser_cases"]
        if case["competitor"] == "servo" and case["mode"] == "native-dom"
    )
    servo["runtime_identity"] = (
        "servo|binary=/opt/servo|version=Servo changed|"
        "webdriver=127.0.0.1:49999|headless=true"
    )
    require_invalid(
        lambda: admit_collection(preflight, webdriver_version_drift, virtual, native),
        "Servo version drift hidden by ephemeral-port normalization",
    )

    zev_system_drift = copy.deepcopy(native)
    zev_system_drift["system_fingerprint"] = "f" * 64
    zev_system_drift["normalized_core_evidence"]["system_fingerprint"] = "f" * 64
    require_invalid(
        lambda: admit_collection(preflight, browser, virtual, zev_system_drift),
        "Zevryon evidence from another system was accepted",
    )

    preflight_failed = copy.deepcopy(preflight)
    ladybird = next(
        record
        for record in preflight_failed["records"]
        if record["competitor"] == "ladybird"
    )
    ladybird["status"] = "unavailable"
    ladybird["launch_ready"] = False
    ladybird["runtime_identity"] = None
    ladybird["reason"] = "Ladybird unavailable"
    preflight_failed["all_runtimes_ready"] = False
    preflight_failed["preflight_gate_passed"] = False
    require_invalid(
        lambda: admit_collection(preflight_failed, browser, virtual, native),
        "failed canonical runtime preflight was accepted",
    )

    browser_corpus_drift = copy.deepcopy(browser)
    browser_corpus_drift["corpus_sha256"] = "0" * 64
    require_invalid(
        lambda: admit_collection(preflight, browser_corpus_drift, virtual, native),
        "browser corpus authority drift was accepted",
    )

    print("M7 collection admission binding tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
