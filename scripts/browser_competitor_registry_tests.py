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


def main() -> int:
    validate_registry()

    require(
        set(CANONICAL_KEYS)
        == {"chrome", "firefox", "edge", "webkit", "servo", "ladybird"},
        "canonical competitor set mismatch",
    )
    require(
        AUXILIARY_KEYS == ("chromium",),
        "auxiliary competitor set mismatch",
    )

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
    require(ladybird.adapter == "ladybird-headless", "Ladybird adapter mismatch")

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
        "success without runtime identity was accepted",
    )
    require_value_error(
        lambda: terminal_record(chrome, "unavailable"),
        "unavailable result without reason was accepted",
    )
    require_value_error(
        lambda: terminal_record(chrome, "not-a-state", reason="test"),
        "unknown terminal state was accepted",
    )

    all_success = [
        terminal_record(
            get_spec(key),
            "success",
            runtime_identity=f"test-{key}-identity",
        )
        for key in CANONICAL_KEYS
    ]
    full_coverage = leadership_coverage(all_success)
    require(full_coverage["full_set_coverage"] is True, "full coverage was rejected")
    require(
        full_coverage["leadership_eligible"] is True,
        "complete successful set was not leadership eligible",
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
        incomplete["leadership_eligible"] is False,
        "unavailable engine incorrectly allowed leadership eligibility",
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

    payload = json.loads(registry_json())
    require(payload["schema"] == "zevryon.competitor.registry.v1", "schema mismatch")
    require(payload["canonical"] == list(CANONICAL_KEYS), "JSON canonical order mismatch")
    require(payload["auxiliary"] == ["chromium"], "JSON auxiliary set mismatch")

    print("Zevryon competitor registry tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
