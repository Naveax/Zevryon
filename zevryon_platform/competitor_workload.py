from __future__ import annotations

from dataclasses import dataclass
from typing import Mapping, Sequence

from zevryon_platform.competitor_lab_v2 import canonical_workload_sha256

WORKLOAD_SCHEMA = "zevryon.m7.workload.v1"
CANONICAL_OPERATION_ORDER = (
    "open_preindexed",
    "open_streaming",
    "scroll",
    "exact_search_warm",
    "exact_search_cold",
    "mutation_batch",
    "copy_all",
)


def _strict_int(value: object, name: str, minimum: int, maximum: int) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise ValueError(f"{name} must be an integer")
    if value < minimum or value > maximum:
        raise ValueError(f"{name} must be in [{minimum}, {maximum}]")
    return value


def _strict_text(value: object, name: str, maximum: int) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{name} must be a string")
    if not value or len(value.encode("utf-8")) > maximum:
        raise ValueError(f"{name} must be non-empty and <= {maximum} UTF-8 bytes")
    return value


def _exact_fields(value: Mapping[str, object], expected: set[str], name: str) -> None:
    if set(value) != expected:
        raise ValueError(f"{name} fields mismatch")


@dataclass(frozen=True)
class CanonicalWorkload:
    corpus_sha256: str
    corpus_logical_bytes: int
    viewport_width_px: int
    viewport_height_px: int
    overscan_px: int
    max_fragments: int
    scroll_samples: int
    scroll_warmup: int
    scroll_step_px: int
    search_query_utf8: str
    warm_search_trials: int
    cold_search_trials: int
    mutation_count: int
    copy_trials: int

    def to_dict(self) -> dict[str, object]:
        return {
            "schema": WORKLOAD_SCHEMA,
            "corpus_sha256": self.corpus_sha256,
            "corpus_logical_bytes": self.corpus_logical_bytes,
            "viewport": {
                "width_px": self.viewport_width_px,
                "height_px": self.viewport_height_px,
                "overscan_px": self.overscan_px,
                "max_fragments": self.max_fragments,
            },
            "operations": [
                {"kind": "open_preindexed"},
                {"kind": "open_streaming"},
                {
                    "kind": "scroll",
                    "samples": self.scroll_samples,
                    "warmup": self.scroll_warmup,
                    "step_px": self.scroll_step_px,
                },
                {
                    "kind": "exact_search_warm",
                    "query_utf8": self.search_query_utf8,
                    "trials": self.warm_search_trials,
                },
                {
                    "kind": "exact_search_cold",
                    "query_utf8": self.search_query_utf8,
                    "trials": self.cold_search_trials,
                    "fresh_process_each_trial": True,
                },
                {"kind": "mutation_batch", "count": self.mutation_count},
                {"kind": "copy_all", "trials": self.copy_trials},
            ],
        }

    @property
    def sha256(self) -> str:
        return canonical_workload_sha256(self.to_dict())


def parse_canonical_workload(value: Mapping[str, object]) -> CanonicalWorkload:
    _exact_fields(
        value,
        {"schema", "corpus_sha256", "corpus_logical_bytes", "viewport", "operations"},
        "workload",
    )
    if value["schema"] != WORKLOAD_SCHEMA:
        raise ValueError("unsupported M7 workload schema")
    corpus_sha = _strict_text(value["corpus_sha256"], "corpus_sha256", 64)
    if len(corpus_sha) != 64 or any(ch not in "0123456789abcdef" for ch in corpus_sha):
        raise ValueError("corpus_sha256 must be lowercase hexadecimal SHA-256")
    corpus_bytes = _strict_int(
        value["corpus_logical_bytes"], "corpus_logical_bytes", 1, (1 << 63) - 1
    )
    viewport = value["viewport"]
    if not isinstance(viewport, Mapping):
        raise ValueError("viewport must be an object")
    _exact_fields(
        viewport,
        {"width_px", "height_px", "overscan_px", "max_fragments"},
        "viewport",
    )
    width = _strict_int(viewport["width_px"], "viewport.width_px", 1, 32768)
    height = _strict_int(viewport["height_px"], "viewport.height_px", 1, 32768)
    overscan = _strict_int(viewport["overscan_px"], "viewport.overscan_px", 0, 131072)
    max_fragments = _strict_int(
        viewport["max_fragments"], "viewport.max_fragments", 1, 1_000_000
    )

    operations = value["operations"]
    if not isinstance(operations, Sequence) or isinstance(operations, (str, bytes)):
        raise ValueError("operations must be an array")
    if len(operations) != len(CANONICAL_OPERATION_ORDER):
        raise ValueError("operations must contain the complete canonical M7 operation set")
    parsed: dict[str, Mapping[str, object]] = {}
    for expected_kind, operation in zip(CANONICAL_OPERATION_ORDER, operations):
        if not isinstance(operation, Mapping):
            raise ValueError("every operation must be an object")
        if operation.get("kind") != expected_kind:
            raise ValueError("operations are not in canonical M7 order")
        parsed[expected_kind] = operation

    _exact_fields(parsed["open_preindexed"], {"kind"}, "open_preindexed")
    _exact_fields(parsed["open_streaming"], {"kind"}, "open_streaming")

    scroll = parsed["scroll"]
    _exact_fields(scroll, {"kind", "samples", "warmup", "step_px"}, "scroll")
    scroll_samples = _strict_int(scroll["samples"], "scroll.samples", 1000, 1_000_000)
    scroll_warmup = _strict_int(scroll["warmup"], "scroll.warmup", 0, 1_000_000)
    if scroll_samples + scroll_warmup > 1_000_000:
        raise ValueError("scroll sample count plus warmup exceeds probe retention cap")
    scroll_step = _strict_int(scroll["step_px"], "scroll.step_px", 1, 1_000_000)

    warm = parsed["exact_search_warm"]
    _exact_fields(warm, {"kind", "query_utf8", "trials"}, "exact_search_warm")
    query = _strict_text(warm["query_utf8"], "search query", 64 * 1024)
    warm_trials = _strict_int(warm["trials"], "warm search trials", 5, 1000)

    cold = parsed["exact_search_cold"]
    _exact_fields(
        cold,
        {"kind", "query_utf8", "trials", "fresh_process_each_trial"},
        "exact_search_cold",
    )
    if cold["query_utf8"] != query:
        raise ValueError("warm and cold exact search must use the same query")
    if cold["fresh_process_each_trial"] is not True:
        raise ValueError("cold search requires a fresh process per trial")
    cold_trials = _strict_int(cold["trials"], "cold search trials", 5, 1000)

    mutation = parsed["mutation_batch"]
    _exact_fields(mutation, {"kind", "count"}, "mutation_batch")
    mutation_count = _strict_int(mutation["count"], "mutation count", 1, 10_000_000)

    copy_op = parsed["copy_all"]
    _exact_fields(copy_op, {"kind", "trials"}, "copy_all")
    copy_trials = _strict_int(copy_op["trials"], "copy trials", 5, 1000)

    result = CanonicalWorkload(
        corpus_sha256=corpus_sha,
        corpus_logical_bytes=corpus_bytes,
        viewport_width_px=width,
        viewport_height_px=height,
        overscan_px=overscan,
        max_fragments=max_fragments,
        scroll_samples=scroll_samples,
        scroll_warmup=scroll_warmup,
        scroll_step_px=scroll_step,
        search_query_utf8=query,
        warm_search_trials=warm_trials,
        cold_search_trials=cold_trials,
        mutation_count=mutation_count,
        copy_trials=copy_trials,
    )
    if result.to_dict() != dict(value):
        raise ValueError("workload is not in canonical normalized form")
    return result
