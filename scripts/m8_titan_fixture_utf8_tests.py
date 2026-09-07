#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import tempfile

import m8_titan_fixture_utf8 as titan_utf8
from generate_massivedoc_corpus import HEADER, MAGIC, RECORD_HEADER


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def validate_every_record_utf8(path: Path, expected_records: int) -> int:
    validated = 0
    with path.open("rb") as handle:
        raw_header = handle.read(HEADER.size)
        require(len(raw_header) == HEADER.size, "Titan UTF-8 smoke header is truncated")
        magic, _, records, *_ = HEADER.unpack(raw_header)
        require(magic == MAGIC, "Titan UTF-8 smoke magic drifted")
        require(records == expected_records, "Titan UTF-8 smoke record count drifted")
        for expected_index in range(records):
            raw = handle.read(RECORD_HEADER.size)
            require(len(raw) == RECORD_HEADER.size, f"record {expected_index} header is truncated")
            logical_id, size = RECORD_HEADER.unpack(raw)
            require(logical_id == expected_index, f"record {expected_index} logical id drifted")
            payload = handle.read(size)
            require(len(payload) == size, f"record {expected_index} payload is truncated")
            try:
                payload.decode("utf-8", errors="strict")
            except UnicodeDecodeError as exc:
                raise TestFailure(f"record {expected_index} is not strict UTF-8: {exc}") from exc
            validated += 1
        require(handle.read(1) == b"", "Titan UTF-8 smoke has trailing bytes")
    return validated


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir is None:
        temp = tempfile.TemporaryDirectory()
        root = Path(temp.name)
    else:
        temp = None
        root = args.work_dir.resolve()
        shutil.rmtree(root, ignore_errors=True)
        root.mkdir(parents=True)

    try:
        config = titan_utf8.base.smoke_config()
        first = root / "first.zmdoc"
        second = root / "second.zmdoc"
        first_generation, first_receipts = titan_utf8.generate_fixture_utf8(first, config)
        second_generation, second_receipts = titan_utf8.generate_fixture_utf8(second, config)
        first_verification = titan_utf8.verify_fixture_utf8(first, config, first_receipts)
        second_verification = titan_utf8.verify_fixture_utf8(second, config, second_receipts)

        require(
            first_verification.get("ordinary_records_utf8_safe_by_construction") is True,
            "UTF-8-safe implementation receipt is missing",
        )
        require(
            first_verification.get("ordinary_record_utf8_implementation") == titan_utf8.UTF8_IMPLEMENTATION,
            "UTF-8 implementation identity drifted",
        )
        require(
            validate_every_record_utf8(first, config.records) == config.records,
            "not every first-run record was UTF-8 validated",
        )
        require(
            validate_every_record_utf8(second, config.records) == config.records,
            "not every replay record was UTF-8 validated",
        )
        require(
            first_generation["container_sha256"] == second_generation["container_sha256"],
            "UTF-8-safe Titan container replay hash drifted",
        )
        require(
            first_generation["payload_sha256"] == second_generation["payload_sha256"],
            "UTF-8-safe Titan payload replay hash drifted",
        )

        # Exercise exact-byte remainders around multi-byte patterns. Every record
        # size must remain exact even when the requested byte count lands inside a
        # code point in the source pattern.
        for index in range(titan_utf8.base.SPECIAL_RECORD_COUNT, titan_utf8.base.SPECIAL_RECORD_COUNT + 24):
            for size in range(1, 64):
                payload = b"".join(titan_utf8.ordinary_utf8_chunks(index, size))
                require(len(payload) == size, f"exact-byte UTF-8 record size drifted for index={index}, size={size}")
                payload.decode("utf-8", errors="strict")
    except (TestFailure, OSError, ValueError, json.JSONDecodeError, titan_utf8.base.TitanEvidenceInvalid) as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        if temp is not None:
            temp.cleanup()
        elif args.work_dir is not None:
            shutil.rmtree(root, ignore_errors=True)

    print("m8-titan-fixture-utf8-tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
