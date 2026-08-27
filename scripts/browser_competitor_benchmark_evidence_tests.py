#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import re

from browser_competitor_benchmark_evidence import (
    CORPUS_CHUNK_BYTES,
    HARNESS_SCHEMA,
    SYNTHETIC_PATTERN,
    SYSTEM_FINGERPRINT_SCHEMA,
    canonical_sha256,
    evidence_identity,
    normalized_system_fingerprint,
    scenario_fingerprint,
    synthetic_corpus_sha256,
)


_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_value_error(callable_, message: str) -> None:
    try:
        callable_()
    except ValueError:
        return
    raise AssertionError(message)


def direct_payload(payload_bytes: int) -> bytes:
    repeats = (CORPUS_CHUNK_BYTES + len(SYNTHETIC_PATTERN) - 1) // len(
        SYNTHETIC_PATTERN
    )
    chunk = (SYNTHETIC_PATTERN * repeats)[:CORPUS_CHUNK_BYTES]
    full_chunks, remainder = divmod(payload_bytes, CORPUS_CHUNK_BYTES)
    return chunk * full_chunks + chunk[:remainder]


def scenario(
    mode: str,
    *,
    virtual_slice_bytes: int = 128 * 1024,
    warmup_query_count: int = 0,
) -> str:
    return scenario_fingerprint(
        mode=mode,
        payload_bytes=64 * 1024 * 1024,
        query_count=21,
        virtual_slice_bytes=virtual_slice_bytes,
        timeout_seconds=180 if mode == "virtualized" else 420,
        warmup_query_count=warmup_query_count,
    )


def canonical_host() -> dict[str, object]:
    return {
        "system_fingerprint_schema": SYSTEM_FINGERPRINT_SCHEMA,
        "machine_metadata_schema": 1,
        "platform": "TestOS",
        "arch": "x86_64",
        "kernel": "1.2.3-test",
        "logical_cpus": 8,
        "cpu_model": "Test CPU 8-Core",
        "physical_ram_mib": 32768,
        "device_class": "desktop",
    }


def main() -> int:
    require(
        canonical_sha256({"b": 2, "a": 1}) == canonical_sha256({"a": 1, "b": 2}),
        "canonical JSON hashing depends on mapping insertion order",
    )

    for payload_bytes in (
        1,
        len(SYNTHETIC_PATTERN),
        CORPUS_CHUNK_BYTES - 1,
        CORPUS_CHUNK_BYTES,
        CORPUS_CHUNK_BYTES + 17,
    ):
        expected = hashlib.sha256(direct_payload(payload_bytes)).hexdigest()
        actual = synthetic_corpus_sha256(payload_bytes)
        require(actual == expected, f"synthetic corpus SHA drift at {payload_bytes} bytes")
        require(_SHA256_RE.fullmatch(actual) is not None, "corpus SHA format drift")

    host = canonical_host()
    first_system = normalized_system_fingerprint(host)
    second_system = normalized_system_fingerprint(dict(reversed(list(host.items()))))
    require(first_system == second_system, "system fingerprint is not canonical")
    require(_SHA256_RE.fullmatch(first_system) is not None, "system SHA format drift")

    changed_cpu = dict(host)
    changed_cpu["cpu_model"] = "Different CPU"
    require(
        normalized_system_fingerprint(changed_cpu) != first_system,
        "system fingerprint ignored CPU model",
    )
    changed_ram = dict(host)
    changed_ram["physical_ram_mib"] = 65536
    require(
        normalized_system_fingerprint(changed_ram) != first_system,
        "system fingerprint ignored physical RAM",
    )
    changed_device_class = dict(host)
    changed_device_class["device_class"] = "modern-phone"
    require(
        normalized_system_fingerprint(changed_device_class) != first_system,
        "system fingerprint ignored device class",
    )

    virtual = scenario("virtualized")
    virtual_again = scenario("virtualized")
    native = scenario("native-dom")
    require(virtual == virtual_again, "scenario fingerprint is not deterministic")
    require(virtual != native, "materially different benchmark modes share an identity")

    native_different_virtual_slice = scenario(
        "native-dom", virtual_slice_bytes=256 * 1024
    )
    require(
        native == native_different_virtual_slice,
        "native-DOM identity depends on virtualized-only slice bytes",
    )
    virtual_different_slice = scenario(
        "virtualized", virtual_slice_bytes=256 * 1024
    )
    require(
        virtual != virtual_different_slice,
        "virtualized identity ignored virtual slice bytes",
    )

    virtual_warm = scenario("virtualized", warmup_query_count=3)
    native_warm = scenario("native-dom", warmup_query_count=3)
    require(
        virtual != virtual_warm,
        "virtualized scenario identity ignored warmup count",
    )
    require(
        native != native_warm,
        "native-DOM scenario identity ignored warmup count",
    )
    require(
        scenario("virtualized", warmup_query_count=3) == virtual_warm,
        "warmup-authoritative scenario identity is not deterministic",
    )

    corpus = synthetic_corpus_sha256(4097)
    identity = evidence_identity(
        host=host,
        corpus_sha256=corpus,
        scenario_sha256=virtual,
    )
    require(identity.host_platform == "TestOS", "host platform identity drift")
    require(identity.host_arch == "x86_64", "host architecture identity drift")
    require(identity.harness_schema == HARNESS_SCHEMA, "harness schema drift")
    require(identity.system_fingerprint == first_system, "system identity drift")
    require(identity.corpus_sha256 == corpus, "corpus identity drift")
    require(identity.scenario_fingerprint == virtual, "scenario identity drift")

    require_value_error(
        lambda: synthetic_corpus_sha256(0),
        "non-positive payload was accepted",
    )
    require_value_error(
        lambda: normalized_system_fingerprint(
            {
                "platform": "TestOS",
                "arch": "x86_64",
                "kernel": "k",
                "logical_cpus": 8,
            }
        ),
        "legacy weak host identity was accepted by fingerprint v2",
    )
    missing_cpu = canonical_host()
    missing_cpu["cpu_model"] = ""
    require_value_error(
        lambda: normalized_system_fingerprint(missing_cpu),
        "blank CPU model was accepted",
    )
    bad_ram = canonical_host()
    bad_ram["physical_ram_mib"] = 128
    require_value_error(
        lambda: normalized_system_fingerprint(bad_ram),
        "implausibly small physical RAM was accepted",
    )
    require_value_error(
        lambda: scenario_fingerprint(
            mode="not-a-mode",
            payload_bytes=1,
            query_count=1,
            virtual_slice_bytes=1,
            timeout_seconds=1,
        ),
        "unknown benchmark mode was accepted",
    )
    require_value_error(
        lambda: scenario_fingerprint(
            mode="virtualized",
            payload_bytes=1,
            query_count=1,
            virtual_slice_bytes=1,
            timeout_seconds=1,
            warmup_query_count=-1,
        ),
        "negative warmup count was accepted",
    )
    require_value_error(
        lambda: scenario_fingerprint(
            mode="virtualized",
            payload_bytes=1,
            query_count=1,
            virtual_slice_bytes=1,
            timeout_seconds=1,
            warmup_query_count=True,
        ),
        "boolean warmup count was accepted",
    )
    require_value_error(
        lambda: evidence_identity(
            host=host,
            corpus_sha256="not-a-sha",
            scenario_sha256=virtual,
        ),
        "invalid corpus digest was accepted",
    )

    print("Zevryon competitor benchmark evidence tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
