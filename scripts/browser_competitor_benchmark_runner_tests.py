#!/usr/bin/env python3
from __future__ import annotations

from browser_competitor_benchmark import _competitor_sets, _validate_case_result
from browser_competitor_benchmark_plan import plan_benchmark_cases
from browser_competitor_registry import get_spec, terminal_record


SHA_A = "a" * 64
SHA_B = "b" * 64
SHA_C = "c" * 64


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def success_result() -> dict[str, object]:
    spec = get_spec("servo")
    return {
        **terminal_record(
            spec,
            "success",
            runtime_identity="servo|binary=/opt/servo|version=test",
            host_platform="TestOS",
            host_arch="x86_64",
            system_fingerprint=SHA_A,
            harness_schema="zevryon.competitor.giant-document.v2",
            corpus_sha256=SHA_B,
            scenario_fingerprint=SHA_C,
        ),
        "browser": "servo",
        "mode": "virtualized",
        "payload_bytes": 4096,
        "query_count": 2,
        "query_details": [
            {"byte_offset": 0, "milliseconds": 1.0},
            {"byte_offset": 1, "milliseconds": 2.0},
        ],
        "memory_metric_status": "valid",
    }


def main() -> int:
    plan = plan_benchmark_cases(["servo"], ["virtualized"])[0]
    valid = _validate_case_result(plan, 4096, success_result())
    require(valid["status"] == "success", "valid success result was rejected")

    drifted = success_result()
    drifted["browser"] = "ladybird"
    invalid_identity = _validate_case_result(plan, 4096, drifted)
    require(invalid_identity["status"] == "invalid", "browser identity drift was accepted")
    require("browser" in str(invalid_identity["reason"]), "identity drift reason lost")

    bad_memory = success_result()
    bad_memory["memory_metric_status"] = "invalid"
    invalid_memory = _validate_case_result(plan, 4096, bad_memory)
    require(invalid_memory["status"] == "invalid", "invalid memory evidence was accepted")

    bad_queries = success_result()
    bad_queries["query_details"] = [{"byte_offset": 0, "milliseconds": 1.0}]
    invalid_queries = _validate_case_result(plan, 4096, bad_queries)
    require(invalid_queries["status"] == "invalid", "inconsistent query evidence was accepted")

    missing_evidence = success_result()
    missing_evidence.pop("scenario_fingerprint")
    invalid_terminal = _validate_case_result(plan, 4096, missing_evidence)
    require(invalid_terminal["status"] == "invalid", "missing terminal evidence was accepted")

    unavailable = {
        **terminal_record(get_spec("servo"), "unavailable", reason="missing binary"),
        "browser": "servo",
        "mode": "virtualized",
        "payload_bytes": 4096,
    }
    validated_unavailable = _validate_case_result(plan, 4096, unavailable)
    require(validated_unavailable["status"] == "unavailable", "valid unavailable state was rewritten")

    sets = _competitor_sets(
        ["servo", "ladybird", "chrome"],
        [
            {"competitor": "servo", "status": "success"},
            {"competitor": "servo", "status": "success"},
            {"competitor": "ladybird", "status": "unavailable"},
            {"competitor": "ladybird", "status": "unavailable"},
            {"competitor": "chrome", "status": "success"},
            {"competitor": "chrome", "status": "error"},
        ],
    )
    require(sets["unavailable_competitors"] == ["ladybird"], "unavailable set drifted")
    require(sets["available_competitors"] == ["servo", "chrome"], "available set drifted")
    require(
        sets["successfully_measured_competitors"] == ["servo", "chrome"],
        "successful set drifted",
    )
    require(sets["fully_measured_competitors"] == ["servo"], "fully measured set drifted")

    print("Zevryon competitor benchmark runner tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
