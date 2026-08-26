#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import math

from browser_competitor_evidence_context import (
    benchmark_scenario,
    build_evidence_context,
    canonical_sha256,
    normalize_benchmark_machine_metadata,
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


def metadata(**overrides: object) -> dict[str, object]:
    value: dict[str, object] = {
        "schema_version": 1,
        "captured_at_utc": "2026-08-26T12:00:00Z",
        "device_class": "desktop",
        "physical_device_confirmed": True,
        "physical_ram_mib": 65536,
        "logical_cpu_count": 16,
        "os_name": "Linux",
        "os_release": "test-kernel",
        "architecture": "x86_64",
        "cpu_model": "test-cpu",
        "run_label": "run-a",
        "thermal": {"state": "nominal", "source": "test", "readings_c": [55.0]},
    }
    value.update(overrides)
    return value


def scenario(payload_bytes: int = 8192, timeout_seconds: int = 180) -> dict[str, object]:
    return benchmark_scenario(
        mode="virtualized",
        payload_bytes=payload_bytes,
        query_count=21,
        virtual_slice_bytes=4096,
        timeout_seconds=timeout_seconds,
        memory_accounting_definition="process-tree-pss-v1",
        harness_schema="zevryon.competitor.giant-document.v2",
    )


def main() -> int:
    normalized = normalize_benchmark_machine_metadata(metadata())
    require("captured_at_utc" not in normalized, "timestamp leaked into system fingerprint input")
    require("run_label" not in normalized, "run label leaked into system fingerprint input")
    changed_receipt = metadata(
        captured_at_utc="2026-08-26T12:30:00Z",
        run_label="run-b",
    )
    require(
        system_state_fingerprint(normalized)
        == system_state_fingerprint(normalize_benchmark_machine_metadata(changed_receipt)),
        "receipt-only metadata changed system fingerprint",
    )
    changed_thermal = metadata(
        thermal={"state": "fair", "source": "test", "readings_c": [65.0]}
    )
    require(
        system_state_fingerprint(normalized)
        != system_state_fingerprint(normalize_benchmark_machine_metadata(changed_thermal)),
        "thermal state change did not affect system fingerprint",
    )
    changed_cpu = metadata(cpu_model="other-cpu")
    require(
        system_state_fingerprint(normalized)
        != system_state_fingerprint(normalize_benchmark_machine_metadata(changed_cpu)),
        "CPU identity change did not affect system fingerprint",
    )
    require_value_error(
        lambda: normalize_benchmark_machine_metadata(metadata(schema_version=2)),
        "unsupported M0 metadata schema was accepted",
    )
    require_value_error(
        lambda: normalize_benchmark_machine_metadata(metadata(physical_device_confirmed="yes")),
        "non-boolean physical flag was accepted",
    )
    require_value_error(
        lambda: normalize_benchmark_machine_metadata(
            metadata(thermal={"state": "nominal", "source": "test", "readings_c": [math.nan]})
        ),
        "NaN thermal evidence was accepted",
    )

    small = 4097
    repeats = (small + len(_PATTERN) - 1) // len(_PATTERN)
    expected = hashlib.sha256((_PATTERN * repeats)[:small]).hexdigest()
    require(unicode_payload_sha256(small) == expected, "Unicode corpus hash drifted")
    require(unicode_payload_sha256(8192) != unicode_payload_sha256(8193), "payload size was ignored")

    common = scenario()
    context = build_evidence_context(
        benchmark_metadata=metadata(),
        harness_schema="zevryon.competitor.giant-document.v2",
        payload_bytes=8192,
        scenario=common,
    )
    require(context.host_platform == "Linux", "host platform did not come from M0 metadata")
    require(context.host_arch == "x86_64", "host arch did not come from M0 metadata")
    record = terminal_record(
        get_spec("chrome"),
        "success",
        runtime_identity=(
            "Google Chrome; adapter=playwright; browser_type=chromium; "
            "channel=chrome; distribution=branded-channel; version=123-test"
        ),
        **context.terminal_kwargs(),
    )
    validate_terminal_record(record)

    require_value_error(
        lambda: build_evidence_context(
            benchmark_metadata=metadata(),
            harness_schema="wrong.schema",
            payload_bytes=8192,
            scenario=common,
        ),
        "mismatched harness schema was accepted",
    )
    require_value_error(
        lambda: build_evidence_context(
            benchmark_metadata=metadata(),
            harness_schema="zevryon.competitor.giant-document.v2",
            payload_bytes=8193,
            scenario=common,
        ),
        "scenario/corpus payload mismatch was accepted",
    )
    require(
        scenario_fingerprint(common) != scenario_fingerprint(scenario(timeout_seconds=181)),
        "timeout policy did not affect scenario fingerprint",
    )
    require_value_error(lambda: canonical_sha256({"bad": math.nan}), "NaN canonical JSON was accepted")

    print("Zevryon competitor evidence context tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
