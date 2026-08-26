#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import math

from browser_competitor_evidence_context import (
    EvidenceContext,
    benchmark_scenario,
    build_evidence_context,
    canonical_json_bytes,
    canonical_sha256,
    capture_host_state,
    scenario_fingerprint,
    system_state_fingerprint,
    unicode_payload_sha256,
)
from browser_competitor_registry import get_spec, terminal_record, validate_terminal_record


_PATTERN = "👨‍👩‍👧‍👦👍🏽🚀 ".encode("utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_value_error(callable_, message: str) -> None:
    try:
        callable_()
    except ValueError:
        return
    raise AssertionError(message)


def repeated_prefix(length: int) -> bytes:
    repeats = (length + len(_PATTERN) - 1) // len(_PATTERN)
    return (_PATTERN * repeats)[:length]


def make_scenario(payload_bytes: int = 8192) -> dict[str, object]:
    return benchmark_scenario(
        mode="virtualized",
        payload_bytes=payload_bytes,
        query_count=21,
        virtual_slice_bytes=4096,
        timeout_seconds=180,
        memory_accounting_definition="process-tree-pss-v1",
        harness_schema="zevryon.competitor.giant-document.v2",
    )


def main() -> int:
    left = {"z": 1, "a": {"β": "🙂", "x": [3, 2, 1]}}
    right = {"a": {"x": [3, 2, 1], "β": "🙂"}, "z": 1}
    require(
        canonical_json_bytes(left) == canonical_json_bytes(right),
        "canonical JSON changed with mapping insertion order",
    )
    require(
        canonical_sha256(left) == canonical_sha256(right),
        "canonical SHA changed with mapping insertion order",
    )
    require(
        canonical_sha256(left) != canonical_sha256({"z": 2, "a": left["a"]}),
        "canonical SHA ignored a value change",
    )
    require_value_error(
        lambda: canonical_sha256({"bad": math.nan}),
        "NaN entered canonical evidence JSON",
    )
    require_value_error(
        lambda: canonical_sha256({1: "non-string-key"}),
        "non-string JSON key entered canonical evidence",
    )

    small_bytes = 4097
    small_expected = hashlib.sha256(repeated_prefix(small_bytes)).hexdigest()
    require(
        unicode_payload_sha256(small_bytes) == small_expected,
        "small generated corpus hash does not match UTF-8 pattern bytes",
    )

    chunk_bytes = 1024 * 1024
    chunk = repeated_prefix(chunk_bytes)
    reset_expected = hashlib.sha256(chunk + chunk[:17]).hexdigest()
    require(
        unicode_payload_sha256(chunk_bytes + 17) == reset_expected,
        "generated corpus hash lost the browser chunk-reset semantics",
    )
    require(
        unicode_payload_sha256(8192) != unicode_payload_sha256(8193),
        "corpus hash ignored payload size",
    )
    require_value_error(
        lambda: unicode_payload_sha256(0),
        "zero-length benchmark corpus was accepted",
    )

    scenario = make_scenario()
    scenario_same = make_scenario()
    scenario_different = benchmark_scenario(
        mode="virtualized",
        payload_bytes=8192,
        query_count=21,
        virtual_slice_bytes=4096,
        timeout_seconds=181,
        memory_accounting_definition="process-tree-pss-v1",
        harness_schema="zevryon.competitor.giant-document.v2",
    )
    require(
        scenario_fingerprint(scenario) == scenario_fingerprint(scenario_same),
        "identical scenario inputs produced different fingerprints",
    )
    require(
        scenario_fingerprint(scenario) != scenario_fingerprint(scenario_different),
        "scenario fingerprint ignored timeout policy",
    )
    require_value_error(
        lambda: benchmark_scenario(
            mode="not-a-mode",
            payload_bytes=1,
            query_count=1,
            virtual_slice_bytes=1,
            timeout_seconds=1,
            memory_accounting_definition="test",
            harness_schema="test.v1",
        ),
        "invalid benchmark mode entered scenario evidence",
    )

    system_a = {
        "platform": "Linux",
        "arch": "x86_64",
        "platform_release": "test-kernel",
        "python_implementation": "CPython",
        "python_version": "3.test",
        "logical_cpus": 8,
    }
    system_b = {
        "logical_cpus": 8,
        "python_version": "3.test",
        "python_implementation": "CPython",
        "platform_release": "test-kernel",
        "arch": "x86_64",
        "platform": "Linux",
    }
    require(
        system_state_fingerprint(system_a) == system_state_fingerprint(system_b),
        "system fingerprint changed with key order",
    )
    require(
        system_state_fingerprint(system_a)
        != system_state_fingerprint({**system_a, "logical_cpus": 16}),
        "system fingerprint ignored system-state change",
    )
    require_value_error(
        lambda: system_state_fingerprint({}),
        "empty system state was accepted",
    )

    actual_platform, actual_arch, actual_state = capture_host_state()
    require(actual_platform == actual_state["platform"], "captured host platform drift")
    require(actual_arch == actual_state["arch"], "captured host architecture drift")
    require(len(system_state_fingerprint(actual_state)) == 64, "captured host fingerprint invalid")

    context = build_evidence_context(
        host_platform="Linux",
        host_arch="x86_64",
        system_state=system_a,
        harness_schema="zevryon.competitor.giant-document.v2",
        payload_bytes=8192,
        scenario=scenario,
    )
    kwargs = context.terminal_kwargs()
    record = terminal_record(
        get_spec("chrome"),
        "success",
        runtime_identity=(
            "Google Chrome; adapter=playwright; browser_type=chromium; "
            "channel=chrome; distribution=branded-channel; version=123-test"
        ),
        **kwargs,
    )
    validate_terminal_record(record)
    require(record["corpus_sha256"] == context.corpus_sha256, "context corpus hash lost")
    require(
        record["scenario_fingerprint"] == context.scenario_fingerprint,
        "context scenario fingerprint lost",
    )

    require_value_error(
        lambda: build_evidence_context(
            host_platform="Linux",
            host_arch="x86_64",
            system_state=system_a,
            harness_schema="wrong.schema",
            payload_bytes=8192,
            scenario=scenario,
        ),
        "mismatched scenario/evidence harness schema was accepted",
    )
    require_value_error(
        lambda: build_evidence_context(
            host_platform="Linux",
            host_arch="x86_64",
            system_state=system_a,
            harness_schema="zevryon.competitor.giant-document.v2",
            payload_bytes=8193,
            scenario=scenario,
        ),
        "scenario/corpus payload mismatch was accepted",
    )
    require_value_error(
        lambda: build_evidence_context(
            host_platform="Windows",
            host_arch="x86_64",
            system_state=system_a,
            harness_schema="zevryon.competitor.giant-document.v2",
            payload_bytes=8192,
            scenario=scenario,
        ),
        "host/system platform mismatch was accepted",
    )
    require_value_error(
        lambda: build_evidence_context(
            host_platform="Linux",
            host_arch="arm64",
            system_state=system_a,
            harness_schema="zevryon.competitor.giant-document.v2",
            payload_bytes=8192,
            scenario=scenario,
        ),
        "host/system architecture mismatch was accepted",
    )

    bad_context = EvidenceContext(
        host_platform="Linux",
        host_arch="x86_64",
        system_fingerprint="A" * 64,
        harness_schema="test.v1",
        corpus_sha256="0" * 64,
        scenario_fingerprint="1" * 64,
    )
    require_value_error(
        bad_context.validate,
        "uppercase/non-canonical SHA-256 was accepted",
    )

    print("Zevryon competitor evidence context tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
