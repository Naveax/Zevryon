#!/usr/bin/env python3
from __future__ import annotations

import json

from browser_competitor_registry import (
    AUXILIARY_KEYS,
    CANONICAL_KEYS,
    get_spec,
    leadership_coverage,
    registry_json,
    resolve_requested,
    terminal_record,
    validate_registry,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_value_error(callable_, message: str) -> None:
    try:
        callable_()
    except ValueError:
        return
    raise AssertionError(message)


def success_record(key: str) -> dict[str, object]:
    return terminal_record(
        get_spec(key),
        "success",
        runtime_identity=f"test-{key}-identity",
        host_platform="linux",
        host_arch="x86_64",
        system_fingerprint="b" * 64,
        harness_schema="zevryon.competitor.giant-document.v2",
        corpus_sha256="a" * 64,
        scenario_fingerprint="c" * 64,
    )


def main() -> int:
    validate_registry()

    require(
        set(CANONICAL_KEYS)
        == {"chrome", "firefox", "edge", "webkit", "servo", "ladybird"},
        "canonical competitor set mismatch",
    )
    require(AUXILIARY_KEYS == ("chromium",), "auxiliary competitor set mismatch")

    chromium = get_spec("chromium")
    chrome = get_spec("chrome")
    edge = get_spec("edge")
    firefox = get_spec("firefox")
    webkit = get_spec("webkit")
    servo = get_spec("servo")
    ladybird = get_spec("ladybird")

    require(not chromium.canonical, "bundled Chromium became canonical")
    require(chrome.playwright_channel == "chrome", "Chrome channel identity mismatch")
    require(edge.playwright_channel == "msedge", "Edge channel identity mismatch")
    require(
        firefox.playwright_browser == "firefox" and firefox.playwright_channel is None,
        "Firefox Playwright identity mismatch",
    )
    require(
        webkit.playwright_browser == "webkit" and webkit.playwright_channel is None,
        "WebKit Playwright identity mismatch",
    )
    require(servo.adapter == "webdriver", "Servo adapter mismatch")
    require(
        servo.launch_hint == "servo --headless --webdriver=PORT about:blank",
        "Servo launch authority mismatch",
    )
    require(ladybird.adapter == "webdriver", "Ladybird adapter mismatch")
    require(
        ladybird.launch_hint == "WebDriver --headless -l 127.0.0.1 -p PORT",
        "Ladybird WebDriver launch authority mismatch",
    )

    require_value_error(
        lambda: resolve_requested(["chrome", "chrome"]),
        "duplicate competitor request was accepted",
    )
    require_value_error(
        lambda: resolve_requested(["not-a-browser"]),
        "unknown competitor request was accepted",
    )
    require_value_error(
        lambda: terminal_record(chrome, "success"),
        "success without required evidence was accepted",
    )
    require_value_error(
        lambda: terminal_record(chrome, "unavailable"),
        "unavailable result without reason was accepted",
    )
    require_value_error(
        lambda: terminal_record(chrome, "not-a-state", reason="test"),
        "unknown terminal state was accepted",
    )

    all_success = [success_record(key) for key in CANONICAL_KEYS]
    full_coverage = leadership_coverage(all_success)
    require(full_coverage["full_set_coverage"] is True, "full coverage was rejected")
    require(full_coverage["comparable_full_set"] is True, "comparable full set was rejected")
    require(
        full_coverage["leadership_evidence_gate_passed"] is True,
        "complete comparable set did not pass leadership evidence gate",
    )
    require(
        full_coverage["leadership_metric_gate_evaluated"] is False,
        "registry incorrectly claimed to evaluate leadership metrics",
    )
    require(
        full_coverage["leadership_eligible"] is False,
        "registry granted final leadership without metric evaluation",
    )

    one_unavailable = list(all_success)
    one_unavailable[-1] = terminal_record(
        get_spec(CANONICAL_KEYS[-1]),
        "unavailable",
        reason="test binary unavailable",
    )
    incomplete = leadership_coverage(one_unavailable)
    require(incomplete["full_set_coverage"] is False, "unavailable engine counted as coverage")
    require(
        incomplete["leadership_evidence_gate_passed"] is False,
        "unavailable engine incorrectly passed leadership evidence gate",
    )

    missing = leadership_coverage(all_success[:-1])
    require(missing["full_set_coverage"] is False, "missing engine counted as coverage")
    require(
        CANONICAL_KEYS[-1] in missing["canonical_missing"],
        "missing canonical engine was not reported",
    )

    require_value_error(
        lambda: leadership_coverage([all_success[0], all_success[0]]),
        "duplicate terminal result was accepted",
    )

    raw_missing_identity = dict(all_success[0])
    raw_missing_identity["runtime_identity"] = None
    require_value_error(
        lambda: leadership_coverage([raw_missing_identity]),
        "raw success without runtime identity bypassed evidence validation",
    )

    raw_missing_system = dict(all_success[0])
    raw_missing_system["system_fingerprint"] = None
    require_value_error(
        lambda: leadership_coverage([raw_missing_system]),
        "raw success without system fingerprint bypassed evidence validation",
    )

    raw_missing_corpus = dict(all_success[0])
    raw_missing_corpus["corpus_sha256"] = None
    require_value_error(
        lambda: leadership_coverage([raw_missing_corpus]),
        "raw success without corpus hash bypassed evidence validation",
    )

    raw_bad_corpus = dict(all_success[0])
    raw_bad_corpus["corpus_sha256"] = "ABC"
    require_value_error(
        lambda: leadership_coverage([raw_bad_corpus]),
        "invalid corpus SHA-256 bypassed evidence validation",
    )

    raw_bad_scenario = dict(all_success[0])
    raw_bad_scenario["scenario_fingerprint"] = "scenario-v1"
    require_value_error(
        lambda: leadership_coverage([raw_bad_scenario]),
        "invalid scenario fingerprint bypassed evidence validation",
    )

    raw_missing_reason = dict(one_unavailable[-1])
    raw_missing_reason["reason"] = None
    require_value_error(
        lambda: leadership_coverage([raw_missing_reason]),
        "raw unsuccessful result without reason bypassed evidence validation",
    )

    raw_spoofed_identity = dict(all_success[0])
    raw_spoofed_identity["canonical_name"] = "Microsoft Edge"
    require_value_error(
        lambda: leadership_coverage([raw_spoofed_identity]),
        "raw result with spoofed canonical identity bypassed registry validation",
    )

    raw_spoofed_adapter = dict(all_success[0])
    raw_spoofed_adapter["adapter"] = "webdriver"
    require_value_error(
        lambda: leadership_coverage([raw_spoofed_adapter]),
        "raw result with spoofed adapter bypassed registry validation",
    )

    different_scenario = [dict(record) for record in all_success]
    different_scenario[-1]["scenario_fingerprint"] = "d" * 64
    mismatch = leadership_coverage(different_scenario)
    require(mismatch["full_set_coverage"] is True, "scenario mismatch lost raw coverage")
    require(mismatch["comparable_full_set"] is False, "scenario mismatch counted as comparable")
    require(
        mismatch["leadership_evidence_gate_passed"] is False,
        "scenario mismatch passed leadership evidence gate",
    )
    require(
        "scenario_fingerprint" in mismatch["comparison_mismatches"],
        "scenario mismatch was not identified",
    )

    different_system = [dict(record) for record in all_success]
    different_system[-1]["system_fingerprint"] = "e" * 64
    system_mismatch = leadership_coverage(different_system)
    require(
        system_mismatch["leadership_evidence_gate_passed"] is False,
        "system mismatch passed leadership evidence gate",
    )
    require(
        "system_fingerprint" in system_mismatch["comparison_mismatches"],
        "system mismatch was not identified",
    )

    different_host = [dict(record) for record in all_success]
    different_host[-1]["host_arch"] = "aarch64"
    host_mismatch = leadership_coverage(different_host)
    require(
        host_mismatch["leadership_evidence_gate_passed"] is False,
        "host mismatch passed leadership evidence gate",
    )
    require(
        "host_arch" in host_mismatch["comparison_mismatches"],
        "host architecture mismatch was not identified",
    )

    payload = json.loads(registry_json())
    require(payload["schema"] == "zevryon.competitor.registry.v1", "schema mismatch")
    require(payload["canonical"] == list(CANONICAL_KEYS), "JSON canonical order mismatch")
    require(payload["auxiliary"] == ["chromium"], "JSON auxiliary set mismatch")
    require(
        "system_fingerprint" in payload["success_evidence_fields"],
        "registry JSON omitted system evidence requirement",
    )
    require(
        "scenario_fingerprint" in payload["success_evidence_fields"],
        "registry JSON omitted scenario evidence requirement",
    )

    print("Zevryon competitor registry tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())