#!/usr/bin/env python3
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
from typing import Iterator

SOURCE_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_ROOT = Path(__file__).resolve().parent
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPT_ROOT))

from generate_massivedoc_corpus import HEADER, MAGIC, RECORD_HEADER, iter_payload_chunks

SCHEMA = "zevryon.m8.titan-fixture.v1"
AUTHORITY = "m8-canonical-titan-fixture-v1"
TAIL_MARKER = b"ZEVRYON_M8_TITAN_TAIL"

CERT_LOGICAL_BYTES = 4 * 1024 * 1024 * 1024
CERT_RECORDS = 8_388_608
CERT_LOGICAL_NODES = 67_108_864
CERT_STYLE_RUNS = 33_554_432
CERT_RESOURCE_REFERENCES = 1_048_576
CERT_GIANT_RECORD_BYTES = 64 * 1024 * 1024
CERT_UNBROKEN_TOKEN_BYTES = 16 * 1024 * 1024
CERT_PATHOLOGICAL_GRAPHEME_BYTES = 64 * 1024

SMOKE_LOGICAL_BYTES = 4 * 1024 * 1024
SMOKE_RECORDS = 4_096
SMOKE_LOGICAL_NODES = 32_768
SMOKE_STYLE_RUNS = 16_384
SMOKE_RESOURCE_REFERENCES = 512
SMOKE_GIANT_RECORD_BYTES = 1 * 1024 * 1024
SMOKE_UNBROKEN_TOKEN_BYTES = 256 * 1024
SMOKE_PATHOLOGICAL_GRAPHEME_BYTES = 4 * 1024

GIANT_RECORD_INDEX = 0
TOKEN_RECORD_INDEX = 1
GRAPHEME_RECORD_INDEX = 2
SPECIAL_RECORD_COUNT = 3
STREAM_CHUNK_BYTES = 1024 * 1024

GIANT_PATTERN = b"g "
TOKEN_BYTE = b"T"
GRAPHEME_BASE = "\u00e9".encode("utf-8")
GRAPHEME_EXTEND = "\u0301".encode("utf-8")


class TitanEvidenceInvalid(RuntimeError):
    pass


@dataclass(frozen=True)
class TitanConfig:
    logical_bytes: int
    records: int
    logical_nodes: int
    style_runs: int
    resource_references: int
    giant_record_bytes: int
    unbroken_token_bytes: int
    pathological_grapheme_bytes: int

    def as_dict(self) -> dict[str, int]:
        return {
            "logical_utf8_bytes": self.logical_bytes,
            "logical_records": self.records,
            "logical_nodes": self.logical_nodes,
            "style_runs": self.style_runs,
            "resource_references": self.resource_references,
            "largest_record_bytes": self.giant_record_bytes,
            "largest_unbroken_token_bytes": self.unbroken_token_bytes,
            "pathological_grapheme_bytes": self.pathological_grapheme_bytes,
        }


def certification_config() -> TitanConfig:
    return TitanConfig(
        CERT_LOGICAL_BYTES,
        CERT_RECORDS,
        CERT_LOGICAL_NODES,
        CERT_STYLE_RUNS,
        CERT_RESOURCE_REFERENCES,
        CERT_GIANT_RECORD_BYTES,
        CERT_UNBROKEN_TOKEN_BYTES,
        CERT_PATHOLOGICAL_GRAPHEME_BYTES,
    )


def smoke_config() -> TitanConfig:
    return TitanConfig(
        SMOKE_LOGICAL_BYTES,
        SMOKE_RECORDS,
        SMOKE_LOGICAL_NODES,
        SMOKE_STYLE_RUNS,
        SMOKE_RESOURCE_REFERENCES,
        SMOKE_GIANT_RECORD_BYTES,
        SMOKE_UNBROKEN_TOKEN_BYTES,
        SMOKE_PATHOLOGICAL_GRAPHEME_BYTES,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TitanEvidenceInvalid(message)


def clean_git_identity() -> tuple[str, str]:
    def run(*args: str) -> str:
        completed = subprocess.run(
            ["git", "-C", str(SOURCE_ROOT), *args],
            text=True,
            encoding="utf-8",
            errors="strict",
            capture_output=True,
            check=False,
            timeout=20.0,
        )
        if completed.returncode != 0:
            raise TitanEvidenceInvalid(
                f"git {' '.join(args)} failed: {completed.stderr.strip()}"
            )
        return completed.stdout.strip()

    require(
        run("status", "--porcelain=v1", "--untracked-files=all") == "",
        "repository must be clean before Titan evidence is generated",
    )
    commit = run("rev-parse", "HEAD")
    tree = run("rev-parse", "HEAD^{tree}")
    require(len(commit) == 40 and all(c in "0123456789abcdef" for c in commit), "invalid Git commit")
    require(len(tree) == 40 and all(c in "0123456789abcdef" for c in tree), "invalid Git tree")
    return commit, tree


def validate_config(config: TitanConfig, *, certification: bool) -> None:
    require(config.records > SPECIAL_RECORD_COUNT, "Titan fixture requires more than three records")
    require(config.logical_bytes > 0, "logical bytes must be positive")
    require(config.logical_nodes >= config.records, "logical nodes must cover every record")
    require(config.style_runs > 0, "style runs must be positive")
    require(config.resource_references > 0, "resource references must be positive")
    require(config.giant_record_bytes > 0, "giant record must be positive")
    require(config.unbroken_token_bytes > 0, "unbroken token must be positive")
    require(config.giant_record_bytes <= 0xFFFFFFFF, "giant record exceeds uint32 ZMDOC storage")
    require(config.unbroken_token_bytes <= config.giant_record_bytes, "unbroken token exceeds giant-record axis")
    require(config.pathological_grapheme_bytes <= config.giant_record_bytes, "pathological grapheme exceeds giant-record axis")
    require(config.pathological_grapheme_bytes >= len(GRAPHEME_BASE), "pathological grapheme is too small")
    require(
        (config.pathological_grapheme_bytes - len(GRAPHEME_BASE)) % len(GRAPHEME_EXTEND) == 0,
        "pathological grapheme byte count must encode one base plus whole combining marks",
    )
    special = config.giant_record_bytes + config.unbroken_token_bytes + config.pathological_grapheme_bytes
    require(special < config.logical_bytes, "special Titan records consume the whole corpus")
    ordinary_records = config.records - SPECIAL_RECORD_COUNT
    ordinary_bytes = config.logical_bytes - special
    require(ordinary_bytes >= ordinary_records, "every ordinary Titan record must contain at least one byte")
    largest_ordinary = (ordinary_bytes + ordinary_records - 1) // ordinary_records
    require(largest_ordinary <= 0xFFFFFFFF, "ordinary record exceeds uint32 ZMDOC storage")
    require(largest_ordinary < config.giant_record_bytes, "ordinary distribution exceeds the giant-record axis")
    require(largest_ordinary >= len(TAIL_MARKER), "last ordinary record cannot contain the frozen tail marker")
    if certification:
        require(config == certification_config(), "certification mode requires the exact frozen Titan configuration")


def ordinary_record_size(index: int, config: TitanConfig) -> int:
    require(index >= SPECIAL_RECORD_COUNT, "ordinary record index overlaps a special Titan record")
    special = config.giant_record_bytes + config.unbroken_token_bytes + config.pathological_grapheme_bytes
    remaining = config.logical_bytes - special
    ordinary_records = config.records - SPECIAL_RECORD_COUNT
    base, extra = divmod(remaining, ordinary_records)
    ordinal = index - SPECIAL_RECORD_COUNT
    return base + (1 if ordinal < extra else 0)


def record_size(index: int, config: TitanConfig) -> int:
    if index == GIANT_RECORD_INDEX:
        return config.giant_record_bytes
    if index == TOKEN_RECORD_INDEX:
        return config.unbroken_token_bytes
    if index == GRAPHEME_RECORD_INDEX:
        return config.pathological_grapheme_bytes
    return ordinary_record_size(index, config)


def repeat_chunks(pattern: bytes, total_bytes: int) -> Iterator[bytes]:
    require(bool(pattern), "repeat pattern cannot be empty")
    remaining = total_bytes
    phase = 0
    while remaining:
        take = min(remaining, STREAM_CHUNK_BYTES)
        repeats = (phase + take + len(pattern) - 1) // len(pattern)
        expanded = pattern * repeats
        yield expanded[phase : phase + take]
        phase = (phase + take) % len(pattern)
        remaining -= take


def pathological_grapheme_payload(total_bytes: int) -> bytes:
    require(total_bytes >= len(GRAPHEME_BASE), "grapheme payload too small")
    remainder = total_bytes - len(GRAPHEME_BASE)
    require(remainder % len(GRAPHEME_EXTEND) == 0, "grapheme payload is not codepoint aligned")
    return GRAPHEME_BASE + GRAPHEME_EXTEND * (remainder // len(GRAPHEME_EXTEND))


def special_chunks(index: int, size: int) -> Iterator[bytes]:
    if index == GIANT_RECORD_INDEX:
        yield from repeat_chunks(GIANT_PATTERN, size)
    elif index == TOKEN_RECORD_INDEX:
        yield from repeat_chunks(TOKEN_BYTE, size)
    elif index == GRAPHEME_RECORD_INDEX:
        yield pathological_grapheme_payload(size)
    else:
        raise TitanEvidenceInvalid(f"unknown special record index: {index}")


def expected_physical_bytes(config: TitanConfig) -> int:
    return HEADER.size + config.records * RECORD_HEADER.size + config.logical_bytes


def write_exclusive_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    try:
        with path.open("x", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    except FileExistsError as exc:
        raise TitanEvidenceInvalid(f"refusing to overwrite report: {path}") from exc


def generate_fixture(
    output: Path,
    config: TitanConfig,
) -> tuple[dict[str, object], dict[int, dict[str, object]]]:
    output.parent.mkdir(parents=True, exist_ok=True)
    payload_hash = hashlib.sha256()
    container_hash = hashlib.sha256()
    special_hashers = {
        GIANT_RECORD_INDEX: hashlib.sha256(),
        TOKEN_RECORD_INDEX: hashlib.sha256(),
        GRAPHEME_RECORD_INDEX: hashlib.sha256(),
    }
    receipts: dict[int, dict[str, object]] = {}
    header = HEADER.pack(
        MAGIC,
        config.logical_bytes,
        config.records,
        config.logical_nodes,
        config.style_runs,
        config.resource_references,
        config.giant_record_bytes,
        0,
    )
    try:
        stream = output.open("xb")
    except FileExistsError as exc:
        raise TitanEvidenceInvalid(f"refusing to overwrite Titan corpus: {output}") from exc

    with stream:
        stream.write(header)
        container_hash.update(header)
        for index in range(config.records):
            size = record_size(index, config)
            header_offset = stream.tell()
            raw_record_header = RECORD_HEADER.pack(index, size)
            stream.write(raw_record_header)
            container_hash.update(raw_record_header)
            payload_offset = stream.tell()
            if index < SPECIAL_RECORD_COUNT:
                chunks = special_chunks(index, size)
            else:
                chunks = iter_payload_chunks(
                    index,
                    size,
                    tail_marker=TAIL_MARKER if index + 1 == config.records else b"",
                    chunk_bytes=STREAM_CHUNK_BYTES,
                )
            written = 0
            for chunk in chunks:
                if not chunk:
                    continue
                stream.write(chunk)
                payload_hash.update(chunk)
                container_hash.update(chunk)
                written += len(chunk)
                if index in special_hashers:
                    special_hashers[index].update(chunk)
            require(written == size, f"record {index} byte count drifted")
            if index in special_hashers:
                receipts[index] = {
                    "record_index": index,
                    "record_header_offset": header_offset,
                    "payload_offset": payload_offset,
                    "payload_bytes": size,
                    "sha256": special_hashers[index].hexdigest(),
                }

    physical = output.stat().st_size
    require(physical == expected_physical_bytes(config), "Titan corpus physical byte count drifted")
    return {
        "container_sha256": container_hash.hexdigest(),
        "payload_sha256": payload_hash.hexdigest(),
        "physical_bytes": physical,
    }, receipts


def verify_special_record(
    handle,
    receipt: dict[str, object],
    expected_index: int,
    expected_size: int,
) -> dict[str, object]:
    header_offset = int(receipt["record_header_offset"])
    payload_offset = int(receipt["payload_offset"])
    handle.seek(header_offset)
    raw = handle.read(RECORD_HEADER.size)
    require(len(raw) == RECORD_HEADER.size, f"special record {expected_index} header is truncated")
    logical_id, size = RECORD_HEADER.unpack(raw)
    require(logical_id == expected_index, f"special record {expected_index} logical id drifted")
    require(size == expected_size, f"special record {expected_index} size drifted")
    require(handle.tell() == payload_offset, f"special record {expected_index} payload offset drifted")

    digest = hashlib.sha256()
    remaining = expected_size
    phase = 0
    payload_ok = True
    while remaining:
        chunk = handle.read(min(remaining, STREAM_CHUNK_BYTES))
        require(bool(chunk), f"special record {expected_index} payload is truncated")
        digest.update(chunk)
        if expected_index == TOKEN_RECORD_INDEX:
            payload_ok = payload_ok and chunk == TOKEN_BYTE * len(chunk)
        elif expected_index == GIANT_RECORD_INDEX:
            for value in chunk:
                if value != GIANT_PATTERN[phase]:
                    payload_ok = False
                    break
                phase = (phase + 1) % len(GIANT_PATTERN)
        remaining -= len(chunk)
    require(digest.hexdigest() == str(receipt["sha256"]), f"special record {expected_index} SHA-256 drifted")
    require(payload_ok, f"special record {expected_index} payload pattern drifted")
    return {
        "record_index": expected_index,
        "record_header_offset": header_offset,
        "payload_offset": payload_offset,
        "payload_bytes": expected_size,
        "sha256": digest.hexdigest(),
    }


def verify_fixture(
    output: Path,
    config: TitanConfig,
    receipts: dict[int, dict[str, object]],
) -> dict[str, object]:
    with output.open("rb") as handle:
        raw_header = handle.read(HEADER.size)
        require(len(raw_header) == HEADER.size, "Titan corpus header is truncated")
        magic, logical_bytes, records, nodes, styles, resources, largest_limit, reserved = HEADER.unpack(raw_header)
        require(magic == MAGIC, "Titan corpus magic drifted")
        require(logical_bytes == config.logical_bytes, "Titan logical byte header drifted")
        require(records == config.records, "Titan record-count header drifted")
        require(nodes == config.logical_nodes, "Titan logical-node header drifted")
        require(styles == config.style_runs, "Titan style-run header drifted")
        require(resources == config.resource_references, "Titan resource-reference header drifted")
        require(largest_limit == config.giant_record_bytes, "Titan largest-record header drifted")
        require(reserved == 0, "Titan reserved header field drifted")

        giant = verify_special_record(handle, receipts[GIANT_RECORD_INDEX], GIANT_RECORD_INDEX, config.giant_record_bytes)
        token = verify_special_record(handle, receipts[TOKEN_RECORD_INDEX], TOKEN_RECORD_INDEX, config.unbroken_token_bytes)
        grapheme = verify_special_record(handle, receipts[GRAPHEME_RECORD_INDEX], GRAPHEME_RECORD_INDEX, config.pathological_grapheme_bytes)
        handle.seek(int(receipts[GRAPHEME_RECORD_INDEX]["payload_offset"]))
        grapheme_raw = handle.read(config.pathological_grapheme_bytes)
        require(
            grapheme_raw == pathological_grapheme_payload(config.pathological_grapheme_bytes),
            "pathological grapheme payload drifted",
        )
        grapheme_codepoints = 1 + (
            (config.pathological_grapheme_bytes - len(GRAPHEME_BASE)) // len(GRAPHEME_EXTEND)
        )

    require(output.stat().st_size == expected_physical_bytes(config), "Titan physical file size drifted")
    return {
        "header_verified": True,
        "physical_size_verified": True,
        "giant_record": {
            **giant,
            "pattern": "ASCII g followed by space, repeated",
            "giant_record_is_not_a_giant_token": True,
        },
        "unbroken_token": {
            **token,
            "token_byte_hex": TOKEN_BYTE.hex(),
            "contains_only_token_byte": True,
        },
        "pathological_grapheme": {
            **grapheme,
            "base_codepoint": "U+00E9",
            "extend_codepoint": "U+0301",
            "codepoints": grapheme_codepoints,
            "single_extended_grapheme_construction": True,
        },
    }


def resolve_config(args: argparse.Namespace) -> TitanConfig:
    base = certification_config() if args.certification else smoke_config()

    def selected(name: str, fallback: int) -> int:
        value = getattr(args, name)
        return fallback if value is None else value

    return TitanConfig(
        selected("logical_bytes", base.logical_bytes),
        selected("records", base.records),
        selected("logical_nodes", base.logical_nodes),
        selected("style_runs", base.style_runs),
        selected("resource_references", base.resource_references),
        selected("giant_record_bytes", base.giant_record_bytes),
        selected("unbroken_token_bytes", base.unbroken_token_bytes),
        selected("pathological_grapheme_bytes", base.pathological_grapheme_bytes),
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Generate and re-open the canonical M8 Titan ZMDOC fixture with explicit "
            "giant-record, unbroken-token and pathological-grapheme receipts."
        )
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--certification", action="store_true")
    parser.add_argument("--logical-bytes", type=int)
    parser.add_argument("--records", type=int)
    parser.add_argument("--logical-nodes", type=int)
    parser.add_argument("--style-runs", type=int)
    parser.add_argument("--resource-references", type=int)
    parser.add_argument("--giant-record-bytes", type=int)
    parser.add_argument("--unbroken-token-bytes", type=int)
    parser.add_argument("--pathological-grapheme-bytes", type=int)
    args = parser.parse_args()

    try:
        output = args.output.resolve()
        report_path = args.report.resolve()
        require(output != report_path, "Titan corpus and report paths must differ")
        require(not output.exists(), f"refusing to overwrite Titan corpus: {output}")
        require(not report_path.exists(), f"refusing to overwrite Titan report: {report_path}")
        config = resolve_config(args)
        validate_config(config, certification=args.certification)
        commit, tree = clean_git_identity()
        generation, receipts = generate_fixture(output, config)
        verification = verify_fixture(output, config, receipts)
        observed = config.as_dict()
        certification_threshold_met = (
            observed["logical_utf8_bytes"] >= CERT_LOGICAL_BYTES
            and observed["logical_records"] >= CERT_RECORDS
            and observed["logical_nodes"] >= CERT_LOGICAL_NODES
            and observed["style_runs"] >= CERT_STYLE_RUNS
            and observed["resource_references"] >= CERT_RESOURCE_REFERENCES
            and observed["largest_record_bytes"] >= CERT_GIANT_RECORD_BYTES
            and observed["largest_unbroken_token_bytes"] >= CERT_UNBROKEN_TOKEN_BYTES
            and observed["pathological_grapheme_bytes"] >= CERT_PATHOLOGICAL_GRAPHEME_BYTES
        )
        gate_passed = bool(
            verification["header_verified"]
            and verification["physical_size_verified"]
            and verification["giant_record"]["giant_record_is_not_a_giant_token"]
            and verification["unbroken_token"]["contains_only_token_byte"]
            and verification["pathological_grapheme"]["single_extended_grapheme_construction"]
        )
        report = {
            "schema": SCHEMA,
            "authority": AUTHORITY,
            "mode": "certification" if args.certification else "smoke",
            "candidate_commit": commit,
            "candidate_tree": tree,
            "corpus_path": str(output),
            "observed_envelope": observed,
            "frozen_certification_envelope": certification_config().as_dict(),
            "generation": generation,
            "verification": verification,
            "certification_threshold_met": certification_threshold_met,
            "certification_eligible": bool(
                args.certification and certification_threshold_met and gate_passed
            ),
            "gate_passed": gate_passed,
        }
        write_exclusive_json(report_path, report)
    except (TitanEvidenceInvalid, OSError, subprocess.SubprocessError, struct.error) as exc:
        print(f"M8 Titan fixture failed: {exc}", file=sys.stderr)
        return 1

    return 0 if report["gate_passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
