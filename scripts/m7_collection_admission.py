#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Mapping

from browser_competitor_benchmark_evidence import normalized_system_fingerprint
from browser_competitor_registry import CANONICAL_KEYS, get_spec
from m7_leadership_evaluator import (
    LeadershipEvaluationInvalid,
    evaluate_leadership,
    validate_zevryon_normalized_case,
)
from m7_normalized_browser_full_set import (
    CanonicalNormalizedBrowserSetInvalid,
    validate_canonical_normalized_browser_report,
)
from m7_physical_host_evidence import (
    PhysicalHostEvidenceInvalid,
    certify_physical_host,
)
from m7_runtime_preflight import (
    RuntimePreflightInvalid,
    validate_runtime_preflight_report,
)


ADMISSION_SCHEMA = "zevryon.competitor.collection-admission.v1"
ADMISSION_AUTHORITY = "m7-preflight-runtime-and-normalized-evidence-binding-v1"
_WEBDRIVER_ENDPOINT_RE = re.compile(r"webdriver=127\.0\.0\.1:(\d+)")


class CollectionAdmissionInvalid(ValueError):
    pass


def stable_runtime_identity(competitor: str, runtime_identity: object) -> str:
    spec = get_spec(competitor)
    if not isinstance(runtime_identity, str) or not runtime_identity.strip():
        raise CollectionAdmissionInvalid(
            f"runtime identity is missing for {competitor}"
        )
    identity = runtime_identity.strip()
    if spec.adapter != "webdriver":
        return identity

    matches = _WEBDRIVER_ENDPOINT_RE.findall(identity)
    if len(matches) != 1:
        raise CollectionAdmissionInvalid(
            f"WebDriver runtime identity must carry exactly one ephemeral endpoint: {competitor}"
        )
    return _WEBDRIVER_ENDPOINT_RE.sub(
        "webdriver=127.0.0.1:<ephemeral>",
        identity,
    )


def _preflight_records(
    preflight: Mapping[str, object],
) -> dict[str, Mapping[str, object]]:
    raw_records = preflight.get("records")
    if not isinstance(raw_records, list):
        raise CollectionAdmissionInvalid("preflight records are missing")
    output: dict[str, Mapping[str, object]] = {}
    for record in raw_records:
        if not isinstance(record, Mapping):
            raise CollectionAdmissionInvalid("preflight record is not an object")
        competitor = str(record.get("competitor", ""))
        if competitor in output:
            raise CollectionAdmissionInvalid(
                f"duplicate preflight competitor: {competitor}"
            )
        output[competitor] = record
    return output


def _browser_cases(
    browser_report: Mapping[str, object],
) -> dict[str, list[Mapping[str, object]]]:
    raw_cases = browser_report.get("browser_cases")
    if not isinstance(raw_cases, list):
        raise CollectionAdmissionInvalid("browser case evidence is missing")
    output = {key: [] for key in CANONICAL_KEYS}
    for case in raw_cases:
        if not isinstance(case, Mapping):
            raise CollectionAdmissionInvalid("browser case is not an object")
        competitor = str(case.get("competitor", ""))
        if competitor not in output:
            raise CollectionAdmissionInvalid(
                f"unexpected browser competitor in admission: {competitor}"
            )
        output[competitor].append(case)
    return output


def bind_runtime_identities(
    preflight: Mapping[str, object],
    browser_report: Mapping[str, object],
) -> dict[str, object]:
    try:
        validate_runtime_preflight_report(preflight)
    except RuntimePreflightInvalid as exc:
        raise CollectionAdmissionInvalid(f"runtime preflight invalid: {exc}") from exc
    if preflight.get("preflight_gate_passed") is not True:
        raise CollectionAdmissionInvalid(
            "runtime preflight did not pass for all canonical competitors"
        )
    try:
        validate_canonical_normalized_browser_report(browser_report)
    except CanonicalNormalizedBrowserSetInvalid as exc:
        raise CollectionAdmissionInvalid(
            f"canonical normalized browser report invalid: {exc}"
        ) from exc

    preflight_system = str(preflight.get("system_fingerprint", ""))
    browser_host = browser_report.get("host")
    if not isinstance(browser_host, Mapping):
        raise CollectionAdmissionInvalid("browser report host metadata is missing")
    try:
        browser_system = normalized_system_fingerprint(browser_host)
    except (TypeError, ValueError) as exc:
        raise CollectionAdmissionInvalid(
            f"browser report host metadata invalid: {exc}"
        ) from exc
    if browser_system != preflight_system:
        raise CollectionAdmissionInvalid(
            "preflight and browser collection system fingerprints differ"
        )

    preflight_records = _preflight_records(preflight)
    browser_cases = _browser_cases(browser_report)
    bindings: dict[str, object] = {}
    for competitor in CANONICAL_KEYS:
        preflight_record = preflight_records.get(competitor)
        if preflight_record is None:
            raise CollectionAdmissionInvalid(
                f"preflight lacks canonical runtime: {competitor}"
            )
        expected_stable = stable_runtime_identity(
            competitor,
            preflight_record.get("runtime_identity"),
        )
        cases = browser_cases[competitor]
        if len(cases) != 2:
            raise CollectionAdmissionInvalid(
                f"browser collection must have two modes for {competitor}"
            )
        measured_raw: list[str] = []
        measured_stable: set[str] = set()
        for case in cases:
            if case.get("system_fingerprint") != preflight_system:
                raise CollectionAdmissionInvalid(
                    f"browser case system fingerprint drifted: {competitor}/{case.get('mode')}"
                )
            raw_identity = case.get("runtime_identity")
            stable = stable_runtime_identity(competitor, raw_identity)
            measured_raw.append(str(raw_identity))
            measured_stable.add(stable)
        if measured_stable != {expected_stable}:
            raise CollectionAdmissionInvalid(
                f"runtime identity changed between preflight and measurement: {competitor}"
            )
        bindings[competitor] = {
            "adapter": get_spec(competitor).adapter,
            "preflight_runtime_identity": preflight_record.get("runtime_identity"),
            "stable_runtime_identity": expected_stable,
            "measurement_runtime_identities": measured_raw,
            "matched": True,
        }
    return bindings


def _physical_host_receipts(
    preflight: Mapping[str, object],
    browser_report: Mapping[str, object],
) -> dict[str, object]:
    preflight_host = preflight.get("host")
    browser_host = browser_report.get("host")
    if not isinstance(preflight_host, Mapping):
        raise CollectionAdmissionInvalid("preflight host metadata is missing")
    if not isinstance(browser_host, Mapping):
        raise CollectionAdmissionInvalid("browser report host metadata is missing")
    try:
        preflight_receipt = certify_physical_host(
            preflight_host,
            label="runtime-preflight",
        )
        browser_receipt = certify_physical_host(
            browser_host,
            label="browser-full-set",
        )
    except PhysicalHostEvidenceInvalid as exc:
        raise CollectionAdmissionInvalid(
            f"physical benchmark host evidence invalid: {exc}"
        ) from exc
    return {
        "runtime_preflight": preflight_receipt,
        "browser_full_set": browser_receipt,
        "physical_host_gate_passed": True,
    }


def admit_collection(
    preflight: Mapping[str, object],
    browser_report: Mapping[str, object],
    zevryon_virtualized: Mapping[str, object],
    zevryon_native_dom: Mapping[str, object],
) -> dict[str, object]:
    runtime_bindings = bind_runtime_identities(preflight, browser_report)
    physical_host_evidence = _physical_host_receipts(preflight, browser_report)
    preflight_system = str(preflight.get("system_fingerprint", ""))

    try:
        zevryon_virtual_normalized = validate_zevryon_normalized_case(
            zevryon_virtualized,
            expected_mode="virtualized",
        )
        zevryon_native_normalized = validate_zevryon_normalized_case(
            zevryon_native_dom,
            expected_mode="native-dom",
        )
    except LeadershipEvaluationInvalid as exc:
        raise CollectionAdmissionInvalid(
            f"Zevryon normalized evidence invalid: {exc}"
        ) from exc

    for mode, evidence in (
        ("virtualized", zevryon_virtual_normalized),
        ("native-dom", zevryon_native_normalized),
    ):
        if evidence.get("system_fingerprint") != preflight_system:
            raise CollectionAdmissionInvalid(
                f"Zevryon {mode} evidence was collected on a different system"
            )

    try:
        evaluation = evaluate_leadership(
            browser_report,
            zevryon_virtualized,
            zevryon_native_dom,
        )
    except LeadershipEvaluationInvalid as exc:
        raise CollectionAdmissionInvalid(
            f"leadership evaluation invalid: {exc}"
        ) from exc

    corpus_sha = zevryon_virtual_normalized.get("corpus_sha256")
    if zevryon_native_normalized.get("corpus_sha256") != corpus_sha:
        raise CollectionAdmissionInvalid("Zevryon mode corpus identities differ")
    if browser_report.get("corpus_sha256") != corpus_sha:
        raise CollectionAdmissionInvalid(
            "browser and Zevryon corpus authorities differ"
        )

    return {
        "schema": ADMISSION_SCHEMA,
        "admission_authority": ADMISSION_AUTHORITY,
        "system_fingerprint": preflight_system,
        "corpus_sha256": corpus_sha,
        "physical_host_evidence": physical_host_evidence,
        "runtime_bindings": runtime_bindings,
        "leadership_evaluation": evaluation,
        "leadership_metric_gate_evaluated": True,
        "leadership_eligible": evaluation.get("leadership_eligible") is True,
    }


def _read_object(path: Path, label: str) -> Mapping[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CollectionAdmissionInvalid(f"cannot read {label}: {exc}") from exc
    if not isinstance(value, Mapping):
        raise CollectionAdmissionInvalid(f"{label} must be a JSON object")
    return value


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Bind physical-host certification, separately collected runtime preflight, "
            "canonical 6x2 browser full set, and both Zevryon normalized modes before "
            "evaluating M7 leadership."
        )
    )
    parser.add_argument("--preflight", type=Path, required=True)
    parser.add_argument("--browser-report", type=Path, required=True)
    parser.add_argument("--zevryon-virtualized", type=Path, required=True)
    parser.add_argument("--zevryon-native-dom", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    inputs = {
        "preflight": args.preflight,
        "browser_report": args.browser_report,
        "zevryon_virtualized": args.zevryon_virtualized,
        "zevryon_native_dom": args.zevryon_native_dom,
    }
    try:
        admission = admit_collection(
            _read_object(args.preflight, "runtime preflight"),
            _read_object(args.browser_report, "browser report"),
            _read_object(args.zevryon_virtualized, "Zevryon virtualized evidence"),
            _read_object(args.zevryon_native_dom, "Zevryon native-DOM evidence"),
        )
        admission["input_artifacts"] = {
            name: {
                "path": str(path),
                "sha256": _sha256_file(path),
            }
            for name, path in inputs.items()
        }
    except (CollectionAdmissionInvalid, OSError) as exc:
        print(f"M7 collection admission rejected: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(admission, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if admission["leadership_eligible"] is True else 2


if __name__ == "__main__":
    raise SystemExit(main())
