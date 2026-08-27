#!/usr/bin/env python3
from __future__ import annotations

import copy

from browser_competitor_benchmark_evidence import EvidenceIdentity, HARNESS_SCHEMA
from browser_competitor_normalized_browser_lifecycle import (
    NORMALIZED_MEMORY_SCOPE,
    NORMALIZED_SETUP_BOUNDARY,
)
from browser_competitor_query_plan import plan_query_offsets
from browser_competitor_scenario_contract import VIEWPORT_HEIGHT, VIEWPORT_WIDTH
import m7_zevryon_normalized_case as module


SHA_A = "a" * 64
SHA_B = "b" * 64
SHA_C = "c" * 64


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_invalid(callable_, message: str) -> None:
    try:
        callable_()
    except module.ZevryonNormalizedCaseInvalid:
        return
    raise AssertionError(message)


def events(mode: str = "virtualized") -> list[dict[str, object]]:
    payload_bytes = 4096
    query_count = 3
    warmup_count = 2
    slice_bytes = 256
    plan = plan_query_offsets(
        mode=mode,
        payload_bytes=payload_bytes,
        virtual_slice_bytes=slice_bytes,
        query_count=query_count,
        warmup_query_count=warmup_count,
    )
    output: list[dict[str, object]] = [
        {
            "schema": module.SESSION_SCHEMA,
            "event": "ready",
            "mode": mode,
            "payload_bytes": payload_bytes,
            "query_count": query_count,
            "warmup_query_count": warmup_count,
            "virtual_slice_bytes": slice_bytes,
            "viewport_width_px": VIEWPORT_WIDTH,
            "viewport_height_px": VIEWPORT_HEIGHT,
            "native_total_height_q8": 100 if mode == "native-dom" else 0,
            "native_checkpoint_bytes": 200 if mode == "native-dom" else 0,
            "internal_setup_seconds": 0.125,
            "source_authority": module.M7_SOURCE_AUTHORITY,
            "record_index": 0,
            "store_payload_sha256": module.synthetic_corpus_sha256(payload_bytes),
            "store_physical_bytes": payload_bytes + 512,
            "normalized_leadership_evidence": False,
        }
    ]
    for ordinal, coordinate in enumerate(plan.measured_offsets):
        event: dict[str, object] = {
            "schema": module.SESSION_SCHEMA,
            "event": "query",
            "ordinal": ordinal,
            "milliseconds": 1.0 + ordinal,
            "source_bytes_read": 256,
            "rendered_height_q8": 300,
            "checkpoint_source_offset": 0 if ordinal == 0 else 128,
            "fragment_count": 4,
            "truncated": False,
        }
        if mode == "virtualized":
            event["byte_offset"] = coordinate
        else:
            event["scroll_fraction_ppm"] = coordinate
        output.append(event)
    output.append(
        {
            "schema": module.SESSION_SCHEMA,
            "event": "complete",
            "query_count": query_count,
            "normalized_leadership_evidence": False,
        }
    )
    return output


def validate(value: list[dict[str, object]], mode: str = "virtualized"):
    return module.validate_session_events(
        value,
        mode=mode,
        payload_bytes=4096,
        query_count=3,
        warmup_query_count=2,
        virtual_slice_bytes=256,
    )


def main() -> int:
    transcript = validate(events())
    require(
        transcript.query_samples_ms == (1.0, 2.0, 3.0),
        "measured query samples drifted",
    )

    native = validate(events("native-dom"), mode="native-dom")
    require(
        native.queries[0].get("scroll_fraction_ppm") is not None,
        "native coordinate evidence was lost",
    )

    wrong_schema = events()
    wrong_schema[0]["schema"] = "legacy"
    require_invalid(
        lambda: validate(wrong_schema),
        "legacy session schema was accepted",
    )

    duplicate_ready = events()
    duplicate_ready.insert(1, copy.deepcopy(duplicate_ready[0]))
    require_invalid(
        lambda: validate(duplicate_ready),
        "duplicate ready event was accepted",
    )

    query_before_ready = events()
    query_before_ready[0], query_before_ready[1] = (
        query_before_ready[1], query_before_ready[0],
    )
    require_invalid(
        lambda: validate(query_before_ready),
        "query before ready was accepted",
    )

    wrong_viewport = events()
    wrong_viewport[0]["viewport_width_px"] = VIEWPORT_WIDTH + 1
    require_invalid(
        lambda: validate(wrong_viewport),
        "viewport drift was accepted",
    )

    prebuilt_source = events()
    prebuilt_source[0]["source_authority"] = "prebuilt-store-diagnostic-v1"
    require_invalid(
        lambda: validate(prebuilt_source),
        "prebuilt Zevryon store was accepted as normalized evidence",
    )

    wrong_store_sha = events()
    wrong_store_sha[0]["store_payload_sha256"] = "0" * 64
    require_invalid(
        lambda: validate(wrong_store_sha),
        "wrong M7 synthetic store SHA was accepted",
    )

    wrong_warmups = events()
    wrong_warmups[0]["warmup_query_count"] = 3
    require_invalid(
        lambda: validate(wrong_warmups),
        "warmup-count drift was accepted",
    )

    wrong_coordinate = events()
    wrong_coordinate[1]["byte_offset"] = 0
    require_invalid(
        lambda: validate(wrong_coordinate),
        "measured query-coordinate drift was accepted",
    )

    negative_timing = events()
    negative_timing[1]["milliseconds"] = -1.0
    require_invalid(
        lambda: validate(negative_timing),
        "negative implementation-local query timing was accepted",
    )

    missing_query = events()
    missing_query.pop(1)
    require_invalid(
        lambda: validate(missing_query),
        "missing measured query was accepted",
    )

    premature_claim = events()
    premature_claim[-1]["normalized_leadership_evidence"] = True
    require_invalid(
        lambda: validate(premature_claim),
        "session executable leadership claim was accepted",
    )

    identity = EvidenceIdentity(
        host_platform="TestOS",
        host_arch="x86_64",
        system_fingerprint=SHA_A,
        harness_schema=HARNESS_SCHEMA,
        corpus_sha256=SHA_B,
        scenario_fingerprint=SHA_C,
    )
    normalized = module.build_normalized_core_evidence(
        identity,
        setup_to_ready_seconds=0.5,
        query_samples_ms=transcript.query_samples_ms,
        warmup_query_count=2,
        incremental_peak_memory_mb=10.0,
        setup_boundary=NORMALIZED_SETUP_BOUNDARY,
        memory_scope=NORMALIZED_MEMORY_SCOPE,
    )
    require(
        normalized["core_metrics"]["query_milliseconds_p50"] == 2.0,
        "normalized P50 drifted",
    )
    require(
        normalized["core_metrics"]["setup_to_ready_seconds"] == 0.5,
        "normalized setup boundary drifted",
    )

    print("M7 Zevryon normalized persistent-session authority tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
