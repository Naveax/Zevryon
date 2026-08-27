#!/usr/bin/env python3
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
from typing import Mapping, Sequence

from browser_competitor_benchmark_evidence import (
    EvidenceIdentity,
    evidence_identity,
    host_metadata,
    scenario_fingerprint,
    synthetic_corpus_sha256,
)
from browser_competitor_normalized_browser_lifecycle import (
    NORMALIZED_MEMORY_SCOPE,
    NORMALIZED_SETUP_BOUNDARY,
)
from browser_competitor_normalized_core_evidence import (
    build_normalized_core_evidence,
    validate_normalized_core_evidence,
)
from browser_competitor_process_scope import (
    BrowserProcessScopeMonitor,
    ProcessScopeSnapshot,
    ProcessScopeUnavailable,
    descendant_identities,
)
from browser_competitor_query_plan import (
    DEFAULT_WARMUP_QUERY_COUNT,
    plan_query_offsets,
)
from browser_competitor_scenario_contract import VIEWPORT_HEIGHT, VIEWPORT_WIDTH


SESSION_SCHEMA = "zevryon.massivedoc.persistent-benchmark-session.v1"
CASE_SCHEMA = "zevryon.massivedoc.normalized-case.v1"
M7_SOURCE_AUTHORITY = "case-owned-m7-synthetic-single-record-v1"


class ZevryonNormalizedCaseInvalid(ValueError):
    pass


class ZevryonNormalizedCaseTimeout(TimeoutError):
    pass


@dataclass(frozen=True)
class SessionTranscript:
    ready: Mapping[str, object]
    queries: tuple[Mapping[str, object], ...]
    complete: Mapping[str, object]

    @property
    def query_samples_ms(self) -> tuple[float, ...]:
        return tuple(float(item["milliseconds"]) for item in self.queries)


@dataclass(frozen=True)
class LiveSessionResult:
    events: tuple[Mapping[str, object], ...]
    setup_to_ready_seconds: float
    setup_snapshot: ProcessScopeSnapshot
    query_snapshot: ProcessScopeSnapshot
    peak_bytes: int
    observed_receipts: tuple[Mapping[str, int], ...]
    stderr: str
    returncode: int


def _nonnegative_int(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ZevryonNormalizedCaseInvalid(f"{field} must be a non-negative integer")
    return value


def _positive_int(value: object, field: str) -> int:
    normalized = _nonnegative_int(value, field)
    if normalized == 0:
        raise ZevryonNormalizedCaseInvalid(f"{field} must be positive")
    return normalized


def _finite_nonnegative(value: object, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ZevryonNormalizedCaseInvalid(f"{field} must be numeric")
    normalized = float(value)
    if not math.isfinite(normalized) or normalized < 0.0:
        raise ZevryonNormalizedCaseInvalid(
            f"{field} must be finite and non-negative"
        )
    return normalized


def _event_schema(event: Mapping[str, object], index: int) -> str:
    if event.get("schema") != SESSION_SCHEMA:
        raise ZevryonNormalizedCaseInvalid(
            f"session event {index} schema mismatch"
        )
    kind = event.get("event")
    if not isinstance(kind, str) or not kind:
        raise ZevryonNormalizedCaseInvalid(
            f"session event {index} lacks an event type"
        )
    return kind


def validate_session_events(
    events: Sequence[Mapping[str, object]],
    *,
    mode: str,
    payload_bytes: int,
    query_count: int,
    warmup_query_count: int,
    virtual_slice_bytes: int,
) -> SessionTranscript:
    if mode not in {"virtualized", "native-dom"}:
        raise ZevryonNormalizedCaseInvalid(f"unknown benchmark mode: {mode}")
    _positive_int(payload_bytes, "payload_bytes")
    _positive_int(query_count, "query_count")
    _nonnegative_int(warmup_query_count, "warmup_query_count")
    _positive_int(virtual_slice_bytes, "virtual_slice_bytes")

    if not events:
        raise ZevryonNormalizedCaseInvalid("persistent session emitted no events")

    ready: Mapping[str, object] | None = None
    complete: Mapping[str, object] | None = None
    queries: list[Mapping[str, object]] = []
    state = "before-ready"
    for index, event in enumerate(events):
        if not isinstance(event, Mapping):
            raise ZevryonNormalizedCaseInvalid(
                f"session event {index} is not an object"
            )
        kind = _event_schema(event, index)
        if kind == "ready":
            if state != "before-ready" or ready is not None:
                raise ZevryonNormalizedCaseInvalid(
                    "persistent session ready event order drifted"
                )
            ready = event
            state = "queries"
        elif kind == "query":
            if state != "queries" or ready is None or complete is not None:
                raise ZevryonNormalizedCaseInvalid(
                    "persistent session query event order drifted"
                )
            queries.append(event)
        elif kind == "complete":
            if state != "queries" or ready is None or complete is not None:
                raise ZevryonNormalizedCaseInvalid(
                    "persistent session complete event order drifted"
                )
            complete = event
            state = "complete"
        else:
            raise ZevryonNormalizedCaseInvalid(
                f"persistent session emitted unknown event: {kind}"
            )

    if ready is None or complete is None or state != "complete":
        raise ZevryonNormalizedCaseInvalid(
            "persistent session did not emit one ready and one complete event"
        )
    if len(queries) != query_count:
        raise ZevryonNormalizedCaseInvalid(
            "persistent session measured query count drifted"
        )

    expected_sha = synthetic_corpus_sha256(payload_bytes)
    expected_ready = {
        "mode": mode,
        "payload_bytes": payload_bytes,
        "query_count": query_count,
        "warmup_query_count": warmup_query_count,
        "virtual_slice_bytes": virtual_slice_bytes,
        "viewport_width_px": VIEWPORT_WIDTH,
        "viewport_height_px": VIEWPORT_HEIGHT,
        "source_authority": M7_SOURCE_AUTHORITY,
        "record_index": 0,
        "store_payload_sha256": expected_sha,
        "normalized_leadership_evidence": False,
    }
    drift = [
        field
        for field, expected in expected_ready.items()
        if ready.get(field) != expected
    ]
    if drift:
        raise ZevryonNormalizedCaseInvalid(
            "persistent session ready authority drifted: " + ", ".join(drift)
        )
    _finite_nonnegative(ready.get("internal_setup_seconds"), "internal_setup_seconds")
    _positive_int(ready.get("store_physical_bytes"), "store_physical_bytes")
    _nonnegative_int(
        ready.get("native_total_height_q8"),
        "native_total_height_q8",
    )
    _nonnegative_int(
        ready.get("native_checkpoint_bytes"),
        "native_checkpoint_bytes",
    )

    plan = plan_query_offsets(
        mode=mode,
        payload_bytes=payload_bytes,
        virtual_slice_bytes=virtual_slice_bytes,
        query_count=query_count,
        warmup_query_count=warmup_query_count,
    )
    for ordinal, (event, expected_coordinate) in enumerate(
        zip(queries, plan.measured_offsets)
    ):
        if event.get("ordinal") != ordinal:
            raise ZevryonNormalizedCaseInvalid(
                f"persistent session query ordinal {ordinal} drifted"
            )
        coordinate_field = (
            "byte_offset" if mode == "virtualized" else "scroll_fraction_ppm"
        )
        if event.get(coordinate_field) != expected_coordinate:
            raise ZevryonNormalizedCaseInvalid(
                f"persistent session query coordinate {ordinal} drifted"
            )
        _finite_nonnegative(
            event.get("milliseconds"),
            f"query[{ordinal}].milliseconds",
        )
        for field in (
            "source_bytes_read",
            "rendered_height_q8",
            "checkpoint_source_offset",
            "fragment_count",
        ):
            _nonnegative_int(event.get(field), f"query[{ordinal}].{field}")
        if not isinstance(event.get("truncated"), bool):
            raise ZevryonNormalizedCaseInvalid(
                f"query[{ordinal}].truncated must be boolean"
            )

    if complete.get("query_count") != query_count:
        raise ZevryonNormalizedCaseInvalid(
            "persistent session complete query count drifted"
        )
    if complete.get("normalized_leadership_evidence") is not False:
        raise ZevryonNormalizedCaseInvalid(
            "persistent session executable claimed normalized leadership evidence"
        )
    return SessionTranscript(
        ready=ready,
        queries=tuple(queries),
        complete=complete,
    )


def sha256_file(path: Path, chunk_bytes: int = 4 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(chunk_bytes)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _reader(
    stream,
    kind: str,
    output: "queue.Queue[tuple[str, str | None]]",
) -> None:
    try:
        for line in stream:
            output.put((kind, line.rstrip("\r\n")))
    finally:
        output.put((kind, None))


def _terminate(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5.0)


def run_live_session(
    command: Sequence[str],
    *,
    timeout_seconds: int,
    expected_query_count: int,
) -> LiveSessionResult:
    _positive_int(timeout_seconds, "timeout_seconds")
    _positive_int(expected_query_count, "expected_query_count")
    root_pid = os.getpid()
    baseline = descendant_identities(root_pid)
    monitor = BrowserProcessScopeMonitor(root_pid, baseline)
    monitor.start()

    process: subprocess.Popen[str] | None = None
    stdout_thread: threading.Thread | None = None
    stderr_thread: threading.Thread | None = None
    messages: "queue.Queue[tuple[str, str | None]]" = queue.Queue()
    events: list[Mapping[str, object]] = []
    stderr_lines: list[str] = []
    setup_snapshot: ProcessScopeSnapshot | None = None
    query_snapshot: ProcessScopeSnapshot | None = None
    setup_to_ready_seconds: float | None = None
    peak_bytes_at_last_query: int | None = None
    observed_at_last_query: tuple[Mapping[str, int], ...] | None = None
    query_events_seen = 0
    setup_started = time.perf_counter()
    deadline = setup_started + timeout_seconds
    stdout_done = False
    stderr_done = False

    try:
        process = subprocess.Popen(
            list(command),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        if process.stdout is None or process.stderr is None:
            raise ZevryonNormalizedCaseInvalid(
                "persistent session pipes were not created"
            )
        stdout_thread = threading.Thread(
            target=_reader,
            args=(process.stdout, "stdout", messages),
            daemon=True,
        )
        stderr_thread = threading.Thread(
            target=_reader,
            args=(process.stderr, "stderr", messages),
            daemon=True,
        )
        stdout_thread.start()
        stderr_thread.start()

        while not (stdout_done and stderr_done and process.poll() is not None):
            remaining = deadline - time.perf_counter()
            if remaining <= 0.0:
                _terminate(process)
                raise ZevryonNormalizedCaseTimeout(
                    f"persistent session exceeded {timeout_seconds} seconds"
                )
            try:
                kind, line = messages.get(timeout=min(0.05, remaining))
            except queue.Empty:
                continue

            if line is None:
                if kind == "stdout":
                    stdout_done = True
                else:
                    stderr_done = True
                continue
            if kind == "stderr":
                stderr_lines.append(line)
                continue
            if not line.strip():
                continue
            try:
                parsed = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ZevryonNormalizedCaseInvalid(
                    f"persistent session emitted non-JSON stdout: {line}"
                ) from exc
            if not isinstance(parsed, dict):
                raise ZevryonNormalizedCaseInvalid(
                    "persistent session stdout event is not an object"
                )
            events.append(parsed)
            event_kind = parsed.get("event")
            if event_kind == "ready":
                if setup_to_ready_seconds is not None:
                    raise ZevryonNormalizedCaseInvalid(
                        "persistent session emitted duplicate ready event"
                    )
                setup_to_ready_seconds = time.perf_counter() - setup_started
                setup_snapshot = monitor.snapshot()
            elif event_kind == "query":
                query_events_seen += 1
                query_snapshot = monitor.snapshot()
                if query_events_seen == expected_query_count:
                    peak_bytes_at_last_query = int(monitor.peak_bytes)
                    observed_at_last_query = tuple(monitor.observed_receipts)
                    monitor.stop()

        returncode = int(process.wait(timeout=1.0))
        if returncode != 0:
            raise ZevryonNormalizedCaseInvalid(
                "persistent session failed "
                f"with exit code {returncode}: {' | '.join(stderr_lines)}"
            )
        if setup_to_ready_seconds is None or setup_snapshot is None:
            raise ZevryonNormalizedCaseInvalid(
                "persistent session never reached the normalized ready boundary"
            )
        if query_snapshot is None or peak_bytes_at_last_query is None:
            raise ZevryonNormalizedCaseInvalid(
                "persistent session did not reach the final query memory boundary"
            )
        if observed_at_last_query is None:
            raise ZevryonNormalizedCaseInvalid(
                "persistent session lost process identity receipts"
            )
        if (
            not setup_snapshot.valid
            or not query_snapshot.valid
            or not observed_at_last_query
        ):
            raise ZevryonNormalizedCaseInvalid(
                "persistent session process scope was empty at a required boundary"
            )
        if peak_bytes_at_last_query <= 0:
            raise ZevryonNormalizedCaseInvalid(
                "persistent session process-scope peak memory is empty"
            )
        return LiveSessionResult(
            events=tuple(events),
            setup_to_ready_seconds=setup_to_ready_seconds,
            setup_snapshot=setup_snapshot,
            query_snapshot=query_snapshot,
            peak_bytes=peak_bytes_at_last_query,
            observed_receipts=observed_at_last_query,
            stderr="\n".join(stderr_lines),
            returncode=returncode,
        )
    finally:
        if process is not None:
            _terminate(process)
        try:
            monitor.stop()
        except BaseException:
            pass
        for thread in (stdout_thread, stderr_thread):
            if thread is not None and thread.is_alive():
                thread.join(timeout=1.0)


def build_success_record(
    *,
    identity: EvidenceIdentity,
    transcript: SessionTranscript,
    live: LiveSessionResult,
    mode: str,
    payload_bytes: int,
    query_count: int,
    warmup_query_count: int,
    virtual_slice_bytes: int,
    timeout_seconds: int,
    session_binary: Path,
) -> dict[str, object]:
    if live.returncode != 0:
        raise ZevryonNormalizedCaseInvalid(
            "successful normalized case requires zero session exit code"
        )
    if live.peak_bytes <= 0:
        raise ZevryonNormalizedCaseInvalid(
            "successful normalized case requires positive process-scope peak"
        )
    binary_sha256 = sha256_file(session_binary)
    normalized = build_normalized_core_evidence(
        identity,
        setup_to_ready_seconds=live.setup_to_ready_seconds,
        query_samples_ms=transcript.query_samples_ms,
        warmup_query_count=warmup_query_count,
        incremental_peak_memory_mb=live.peak_bytes / 1_000_000,
        setup_boundary=NORMALIZED_SETUP_BOUNDARY,
        memory_scope=NORMALIZED_MEMORY_SCOPE,
    )
    validate_normalized_core_evidence(normalized)
    return {
        "schema": CASE_SCHEMA,
        "implementation": "zevryon",
        "status": "success",
        "mode": mode,
        "payload_bytes": payload_bytes,
        "query_count": query_count,
        "warmup_query_count": warmup_query_count,
        "virtual_slice_bytes": (
            virtual_slice_bytes if mode == "virtualized" else None
        ),
        "timeout_seconds": timeout_seconds,
        "source_authority": M7_SOURCE_AUTHORITY,
        "record_index": 0,
        "runtime_identity": (
            "zevryon|persistent-session|sha256=" + binary_sha256
        ),
        "session_binary": str(session_binary.resolve()),
        "session_binary_sha256": binary_sha256,
        **identity.as_terminal_kwargs(),
        "normalized_setup_to_ready_seconds": live.setup_to_ready_seconds,
        "process_scope_peak_mb": live.peak_bytes / 1_000_000,
        "process_scope_resident_mb_at_ready": (
            live.setup_snapshot.resident_bytes / 1_000_000
        ),
        "process_scope_resident_mb_after_last_query": (
            live.query_snapshot.resident_bytes / 1_000_000
        ),
        "process_scope_ready_receipts": live.setup_snapshot.receipts,
        "process_scope_last_query_receipts": live.query_snapshot.receipts,
        "process_scope_observed_receipts": list(live.observed_receipts),
        "session_ready": dict(transcript.ready),
        "query_details": [dict(item) for item in transcript.queries],
        "session_complete": dict(transcript.complete),
        "session_stderr": live.stderr,
        "normalized_core_evidence": normalized,
    }


def run_normalized_case(
    *,
    session_binary: Path,
    mode: str,
    payload_bytes: int,
    query_count: int,
    warmup_query_count: int,
    virtual_slice_bytes: int,
    timeout_seconds: int,
    max_fragments: int = 512,
) -> dict[str, object]:
    if not session_binary.is_file():
        raise ZevryonNormalizedCaseInvalid(
            f"persistent session binary does not exist: {session_binary}"
        )
    _positive_int(max_fragments, "max_fragments")
    _positive_int(timeout_seconds, "timeout_seconds")

    host = host_metadata()
    corpus_sha256 = synthetic_corpus_sha256(payload_bytes)
    scenario_sha256 = scenario_fingerprint(
        mode=mode,
        payload_bytes=payload_bytes,
        query_count=query_count,
        warmup_query_count=warmup_query_count,
        virtual_slice_bytes=virtual_slice_bytes,
        timeout_seconds=timeout_seconds,
    )
    identity = evidence_identity(
        host=host,
        corpus_sha256=corpus_sha256,
        scenario_sha256=scenario_sha256,
    )

    with tempfile.TemporaryDirectory(prefix="zevryon-m7-normalized-") as case_root:
        command = [
            str(session_binary),
            "--m7-synthetic",
            mode,
            case_root,
            str(payload_bytes),
            str(query_count),
            str(warmup_query_count),
            str(virtual_slice_bytes),
            str(VIEWPORT_WIDTH),
            str(VIEWPORT_HEIGHT),
            str(max_fragments),
        ]
        live = run_live_session(
            command,
            timeout_seconds=timeout_seconds,
            expected_query_count=query_count,
        )
        transcript = validate_session_events(
            live.events,
            mode=mode,
            payload_bytes=payload_bytes,
            query_count=query_count,
            warmup_query_count=warmup_query_count,
            virtual_slice_bytes=virtual_slice_bytes,
        )

    return build_success_record(
        identity=identity,
        transcript=transcript,
        live=live,
        mode=mode,
        payload_bytes=payload_bytes,
        query_count=query_count,
        warmup_query_count=warmup_query_count,
        virtual_slice_bytes=virtual_slice_bytes,
        timeout_seconds=timeout_seconds,
        session_binary=session_binary,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Collect normalized M7 Zevryon persistent-session evidence. "
            "The case-owned process constructs the exact single-record M7 store "
            "after launch so implementation-specific store preparation is charged "
            "to setup."
        )
    )
    parser.add_argument("--session-binary", type=Path, required=True)
    parser.add_argument(
        "--mode",
        choices=("virtualized", "native-dom"),
        required=True,
    )
    parser.add_argument("--payload-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--query-count", type=int, default=21)
    parser.add_argument(
        "--warmup-query-count",
        type=int,
        default=DEFAULT_WARMUP_QUERY_COUNT,
    )
    parser.add_argument("--virtual-slice-bytes", type=int, default=128 * 1024)
    parser.add_argument("--timeout-seconds", type=int, required=True)
    parser.add_argument("--max-fragments", type=int, default=512)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    try:
        record = run_normalized_case(
            session_binary=args.session_binary,
            mode=args.mode,
            payload_bytes=args.payload_bytes,
            query_count=args.query_count,
            warmup_query_count=args.warmup_query_count,
            virtual_slice_bytes=args.virtual_slice_bytes,
            timeout_seconds=args.timeout_seconds,
            max_fragments=args.max_fragments,
        )
    except (
        OSError,
        ProcessScopeUnavailable,
        ZevryonNormalizedCaseInvalid,
        ZevryonNormalizedCaseTimeout,
    ) as exc:
        print(f"Zevryon normalized case failed: {exc}", file=sys.stderr)
        return 1

    text = json.dumps(record, indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
