#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Callable, Mapping

from browser_competitor_benchmark_evidence import (
    host_metadata,
    normalized_system_fingerprint,
)
from browser_competitor_case_executor import webdriver_failure_status
from browser_competitor_playwright import (
    launch_browser,
    launch_failure_status,
    runtime_identity as playwright_runtime_identity,
)
from browser_competitor_registry import CANONICAL_KEYS, get_spec
from browser_competitor_webdriver_runtime import (
    launch_prepared_runtime,
    prepare_webdriver_runtime,
)


PREFLIGHT_SCHEMA = "zevryon.competitor.runtime-preflight.v1"
PREFLIGHT_AUTHORITY = "canonical-exact-runtime-launch-readiness-v1"
PREFLIGHT_STATES = frozenset({"success", "unavailable", "error", "invalid"})


class RuntimePreflightInvalid(ValueError):
    pass


def _failure_record(
    competitor: str,
    status: str,
    reason: str,
) -> dict[str, object]:
    spec = get_spec(competitor)
    if status not in PREFLIGHT_STATES or status == "success":
        raise RuntimePreflightInvalid(f"invalid preflight failure state: {status}")
    if not reason.strip():
        raise RuntimePreflightInvalid("preflight failure requires a reason")
    return {
        "competitor": competitor,
        "canonical_name": spec.canonical_name,
        "adapter": spec.adapter,
        "status": status,
        "launch_ready": False,
        "runtime_identity": None,
        "reason": reason.strip(),
    }


def _success_record(
    competitor: str,
    runtime_identity: str,
    **extra: object,
) -> dict[str, object]:
    spec = get_spec(competitor)
    identity = runtime_identity.strip()
    if not identity:
        raise RuntimePreflightInvalid("successful preflight requires runtime identity")
    return {
        "competitor": competitor,
        "canonical_name": spec.canonical_name,
        "adapter": spec.adapter,
        "status": "success",
        "launch_ready": True,
        "runtime_identity": identity,
        "reason": None,
        **extra,
    }


def probe_playwright_runtime(competitor: str) -> dict[str, object]:
    spec = get_spec(competitor)
    if spec.adapter != "playwright":
        raise RuntimePreflightInvalid(
            f"Playwright probe cannot inspect adapter {spec.adapter}: {competitor}"
        )
    try:
        from playwright.sync_api import sync_playwright
    except ImportError as exc:
        return _failure_record(
            competitor,
            "unavailable",
            f"Playwright Python runtime unavailable: {exc}",
        )

    browser = None
    try:
        with sync_playwright() as playwright:
            resolved_spec, browser, plan = launch_browser(playwright, competitor)
            identity = playwright_runtime_identity(
                resolved_spec,
                plan,
                str(browser.version),
            )
            return _success_record(
                competitor,
                identity,
                browser_type=plan.browser_type,
                channel=plan.channel,
                distribution=plan.distribution,
            )
    except BaseException as exc:
        status, reason = launch_failure_status(exc)
        if status not in PREFLIGHT_STATES:
            status = "error"
        return _failure_record(competitor, status, reason)
    finally:
        if browser is not None:
            try:
                browser.close()
            except BaseException:
                pass


def probe_webdriver_runtime(competitor: str) -> dict[str, object]:
    spec = get_spec(competitor)
    if spec.adapter != "webdriver":
        raise RuntimePreflightInvalid(
            f"WebDriver probe cannot inspect adapter {spec.adapter}: {competitor}"
        )
    handle = None
    try:
        prepared = prepare_webdriver_runtime(competitor)
        handle = launch_prepared_runtime(prepared)
        return _success_record(
            competitor,
            prepared.runtime_identity,
            identity_source=prepared.identity_source,
            webdriver_status=dict(handle.status_receipt),
        )
    except BaseException as exc:
        status, reason = webdriver_failure_status(exc)
        if status == "unsupported":
            status = "error"
        if status not in PREFLIGHT_STATES:
            status = "error"
        return _failure_record(competitor, status, reason)
    finally:
        if handle is not None:
            try:
                handle.close()
            except BaseException:
                pass


Probe = Callable[[str], dict[str, object]]


def run_runtime_preflight(
    *,
    playwright_probe: Probe = probe_playwright_runtime,
    webdriver_probe: Probe = probe_webdriver_runtime,
) -> dict[str, object]:
    host = host_metadata()
    system_fingerprint = normalized_system_fingerprint(host)
    records: list[dict[str, object]] = []
    for competitor in CANONICAL_KEYS:
        spec = get_spec(competitor)
        if spec.adapter == "playwright":
            record = playwright_probe(competitor)
        elif spec.adapter == "webdriver":
            record = webdriver_probe(competitor)
        else:
            record = _failure_record(
                competitor,
                "invalid",
                f"canonical competitor adapter is not preflightable: {spec.adapter}",
            )
        records.append(record)

    all_ready = all(record.get("status") == "success" for record in records)
    report: dict[str, object] = {
        "schema": PREFLIGHT_SCHEMA,
        "preflight_authority": PREFLIGHT_AUTHORITY,
        "host": host,
        "system_fingerprint": system_fingerprint,
        "canonical_competitors": list(CANONICAL_KEYS),
        "records": records,
        "all_runtimes_ready": all_ready,
        "preflight_gate_passed": all_ready,
        "measurement_started": False,
    }
    validate_runtime_preflight_report(report)
    return report


def validate_runtime_preflight_report(report: Mapping[str, object]) -> None:
    if report.get("schema") != PREFLIGHT_SCHEMA:
        raise RuntimePreflightInvalid("runtime preflight schema mismatch")
    if report.get("preflight_authority") != PREFLIGHT_AUTHORITY:
        raise RuntimePreflightInvalid("runtime preflight authority mismatch")
    host = report.get("host")
    if not isinstance(host, Mapping):
        raise RuntimePreflightInvalid("runtime preflight host metadata is missing")
    try:
        expected_system_fingerprint = normalized_system_fingerprint(host)
    except (TypeError, ValueError) as exc:
        raise RuntimePreflightInvalid(
            f"runtime preflight host metadata is invalid: {exc}"
        ) from exc
    if report.get("system_fingerprint") != expected_system_fingerprint:
        raise RuntimePreflightInvalid("runtime preflight system fingerprint drifted")
    if report.get("canonical_competitors") != list(CANONICAL_KEYS):
        raise RuntimePreflightInvalid("runtime preflight canonical set drifted")
    if report.get("measurement_started") is not False:
        raise RuntimePreflightInvalid("runtime preflight cannot claim benchmark measurement")

    raw_records = report.get("records")
    if not isinstance(raw_records, list) or len(raw_records) != len(CANONICAL_KEYS):
        raise RuntimePreflightInvalid("runtime preflight record count drifted")

    discovered: dict[str, Mapping[str, object]] = {}
    for index, record in enumerate(raw_records):
        if not isinstance(record, Mapping):
            raise RuntimePreflightInvalid(f"runtime preflight record {index} is not an object")
        competitor = record.get("competitor")
        if competitor not in CANONICAL_KEYS:
            raise RuntimePreflightInvalid(
                f"runtime preflight record {index} is not canonical"
            )
        key = str(competitor)
        if key in discovered:
            raise RuntimePreflightInvalid(f"duplicate runtime preflight record: {key}")
        spec = get_spec(key)
        if record.get("canonical_name") != spec.canonical_name:
            raise RuntimePreflightInvalid(f"runtime preflight canonical name drifted: {key}")
        if record.get("adapter") != spec.adapter:
            raise RuntimePreflightInvalid(f"runtime preflight adapter drifted: {key}")
        status = record.get("status")
        if status not in PREFLIGHT_STATES:
            raise RuntimePreflightInvalid(f"runtime preflight status invalid: {key}")
        if status == "success":
            identity = record.get("runtime_identity")
            if not isinstance(identity, str) or not identity.strip():
                raise RuntimePreflightInvalid(
                    f"successful runtime preflight lacks identity: {key}"
                )
            if record.get("launch_ready") is not True:
                raise RuntimePreflightInvalid(
                    f"successful runtime preflight is not launch-ready: {key}"
                )
            if record.get("reason") is not None:
                raise RuntimePreflightInvalid(
                    f"successful runtime preflight carries failure reason: {key}"
                )
        else:
            reason = record.get("reason")
            if not isinstance(reason, str) or not reason.strip():
                raise RuntimePreflightInvalid(
                    f"failed runtime preflight lacks reason: {key}"
                )
            if record.get("launch_ready") is not False:
                raise RuntimePreflightInvalid(
                    f"failed runtime preflight claimed launch readiness: {key}"
                )
            if record.get("runtime_identity") is not None:
                raise RuntimePreflightInvalid(
                    f"failed runtime preflight carries admitted identity: {key}"
                )
        discovered[key] = record

    if set(discovered) != set(CANONICAL_KEYS):
        raise RuntimePreflightInvalid("runtime preflight exact canonical set is incomplete")
    expected_all_ready = all(
        discovered[key].get("status") == "success" for key in CANONICAL_KEYS
    )
    if report.get("all_runtimes_ready") is not expected_all_ready:
        raise RuntimePreflightInvalid("runtime preflight all-ready summary drifted")
    if report.get("preflight_gate_passed") is not expected_all_ready:
        raise RuntimePreflightInvalid("runtime preflight gate summary drifted")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Launch-check the exact six canonical M7 browser/engine runtimes without "
            "starting benchmark measurement. Run this as a separate readiness stage, "
            "not immediately coupled to canonical timing collection."
        )
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        report = run_runtime_preflight()
    except (RuntimePreflightInvalid, ValueError) as exc:
        print(f"M7 runtime preflight rejected: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if report["preflight_gate_passed"] is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
