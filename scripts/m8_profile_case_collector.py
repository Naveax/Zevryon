#!/usr/bin/env python3
from __future__ import annotations

import argparse
import codecs
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from typing import Any, Mapping

ROOT = Path(__file__).resolve().parents[1]
SCRIPT_ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPT_ROOT))

from browser_competitor_benchmark_evidence import host_metadata, normalized_system_fingerprint
from m7_physical_host_evidence import (
    PHYSICAL_HOST_AUTHORITY,
    PhysicalHostEvidenceInvalid,
    certify_physical_host,
)
from zevryon_platform.performance_contract import DEVICE_PROFILES, DeviceClass

CASE_SCHEMA = "zevryon.m8.profile-case.v1"
CASE_AUTHORITY = "m8-raw-profile-case-v1"
PROVENANCE_SCHEMA = "zevryon.m8.profile-case-provenance.v1"
PROVENANCE_AUTHORITY = "m8-profile-case-collector-v1"
HOST_SCHEMA = "zevryon.m8.profile-physical-host.v1"
FAILURE_SCHEMA = "zevryon.m8.profile-case-failure.v1"
PROBE_SCHEMA = "zevryon.m8.profile-probe.v1"
TITAN_SCHEMA = "zevryon.m8.titan-fixture.v1"
TITAN_AUTHORITY = "m8-canonical-titan-fixture-v1"
PSS_AUTHORITY = "linux-procfs-process-group-smaps-rollup-v1"
SAMPLE_INTERVAL_SECONDS = 0.05
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")


class ProfileCaseInvalid(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ProfileCaseInvalid(message)


def canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path, chunk_bytes: int = 4 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(chunk_bytes)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def strict_json(path: Path, label: str) -> dict[str, Any]:
    try:
        text = path.read_text(encoding="utf-8", errors="strict")
        value = json.loads(
            text,
            parse_constant=lambda token: (_ for _ in ()).throw(
                ProfileCaseInvalid(f"{label} contains non-finite JSON constant: {token}")
            ),
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProfileCaseInvalid(f"{label} is not strict UTF-8 JSON: {exc}") from exc
    require(isinstance(value, dict), f"{label} must be a JSON object")
    return value


def strict_json_text(text: str, label: str) -> dict[str, Any]:
    try:
        value = json.loads(
            text,
            parse_constant=lambda token: (_ for _ in ()).throw(
                ProfileCaseInvalid(f"{label} contains non-finite JSON constant: {token}")
            ),
        )
    except json.JSONDecodeError as exc:
        raise ProfileCaseInvalid(f"{label} is invalid JSON: {exc}") from exc
    require(isinstance(value, dict), f"{label} must be a JSON object")
    return value


def write_exclusive_json(path: Path, value: object) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")
    try:
        with path.open("xb") as handle:
            handle.write(raw)
    except FileExistsError as exc:
        raise ProfileCaseInvalid(f"refusing to overwrite evidence: {path}") from exc
    return sha256_bytes(raw)


def clean_git_identity() -> tuple[str, str]:
    def run(*args: str) -> str:
        completed = subprocess.run(
            ["git", "-C", str(ROOT), *args],
            text=True,
            encoding="utf-8",
            errors="strict",
            capture_output=True,
            check=False,
            timeout=20.0,
        )
        if completed.returncode != 0:
            raise ProfileCaseInvalid(
                f"git {' '.join(args)} failed: {completed.stderr.strip()}"
            )
        return completed.stdout.strip()

    require(
        run("status", "--porcelain=v1", "--untracked-files=all") == "",
        "repository must be clean before profile evidence is collected",
    )
    commit = run("rev-parse", "HEAD")
    tree = run("rev-parse", "HEAD^{tree}")
    require(HEX40.fullmatch(commit) is not None, "invalid Git commit identity")
    require(HEX40.fullmatch(tree) is not None, "invalid Git tree identity")
    return commit, tree


def ensure_fresh_attempt_dir(path: Path) -> None:
    require(not path.exists(), f"profile attempt directory already exists: {path}")
    path.mkdir(parents=True, exist_ok=False)


def load_certification_titan(report_path: Path, corpus_path: Path) -> tuple[dict[str, Any], str]:
    report = strict_json(report_path, "Titan report")
    require(report.get("schema") == TITAN_SCHEMA, "Titan report schema mismatch")
    require(report.get("authority") == TITAN_AUTHORITY, "Titan report authority mismatch")
    require(report.get("mode") == "certification", "profile collection requires certification-mode Titan evidence")
    require(report.get("certification_threshold_met") is True, "Titan certification threshold is not met")
    require(report.get("certification_eligible") is True, "Titan report is not certification eligible")
    require(report.get("gate_passed") is True, "Titan fixture gate did not pass")
    generation = report.get("generation")
    require(isinstance(generation, dict), "Titan generation receipt is missing")
    container_sha = generation.get("container_sha256")
    payload_sha = generation.get("payload_sha256")
    require(isinstance(container_sha, str) and HEX64.fullmatch(container_sha) is not None, "Titan container SHA-256 is invalid")
    require(isinstance(payload_sha, str) and HEX64.fullmatch(payload_sha) is not None, "Titan payload SHA-256 is invalid")
    actual_container_sha = sha256_file(corpus_path)
    require(actual_container_sha == container_sha, "Titan corpus SHA-256 differs from its report")
    return report, sha256_file(report_path)


def validate_no_physical_identity_overrides() -> None:
    forbidden = {
        "ZEVRYON_PHYSICAL_RAM_MIB": "physical RAM override",
        "ZEVRYON_DEVICE_PROFILE": "device-profile override",
    }
    active = [label for name, label in forbidden.items() if os.environ.get(name)]
    require(not active, "physical certification forbids environment overrides: " + ", ".join(active))


def certified_host_snapshot(label: str, requested_profile: DeviceClass) -> tuple[dict[str, object], dict[str, object], str]:
    host = host_metadata()
    try:
        receipt = certify_physical_host(host, label=label)
        fingerprint = normalized_system_fingerprint(host)
    except (PhysicalHostEvidenceInvalid, TypeError, ValueError) as exc:
        raise ProfileCaseInvalid(f"physical host certification failed: {exc}") from exc
    require(host.get("device_class") == requested_profile.value, "requested profile differs from the physical host device class")
    metadata = host.get("benchmark_machine_metadata")
    require(isinstance(metadata, dict), "physical host machine metadata is missing")
    require(metadata.get("device_class") == requested_profile.value, "machine receipt device class mismatch")
    profile = DEVICE_PROFILES[requested_profile]
    physical_ram = host.get("physical_ram_mib")
    require(type(physical_ram) is int and physical_ram >= profile.minimum_physical_ram_mib, "physical RAM is below the requested profile minimum")
    thermal = receipt.get("thermal")
    require(isinstance(thermal, dict), "physical thermal receipt is missing")
    require(thermal.get("state") not in {"serious", "critical"}, "physical host thermal state is not admission-stable")
    return host, receipt, fingerprint


def _process_group_id_from_stat(path: Path) -> int | None:
    try:
        text = path.read_text(encoding="ascii", errors="strict")
    except (FileNotFoundError, PermissionError, ProcessLookupError, OSError):
        return None
    close = text.rfind(")")
    if close < 0:
        return None
    fields = text[close + 1 :].strip().split()
    if len(fields) < 3:
        return None
    try:
        return int(fields[2])
    except ValueError:
        return None


def process_group_pids(pgid: int) -> list[int]:
    pids: list[int] = []
    proc = Path("/proc")
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        found = _process_group_id_from_stat(entry / "stat")
        if found == pgid:
            pids.append(int(entry.name))
    return sorted(pids)


def read_pss_kib(pid: int) -> int:
    path = Path(f"/proc/{pid}/smaps_rollup")
    try:
        lines = path.read_text(encoding="ascii", errors="strict").splitlines()
    except (FileNotFoundError, PermissionError, ProcessLookupError, OSError) as exc:
        raise ProfileCaseInvalid(f"cannot read PSS for process {pid}: {exc}") from exc
    for line in lines:
        if line.startswith("Pss:"):
            parts = line.split()
            require(len(parts) >= 2, f"malformed Pss line for process {pid}")
            try:
                value = int(parts[1])
            except ValueError as exc:
                raise ProfileCaseInvalid(f"non-numeric Pss for process {pid}") from exc
            require(value >= 0, f"negative Pss for process {pid}")
            return value
    raise ProfileCaseInvalid(f"smaps_rollup for process {pid} has no Pss field")


def sample_process_group(pgid: int, expected_root_pid: int, started_ns: int) -> dict[str, object]:
    pids = process_group_pids(pgid)
    require(pids, "profile probe process group disappeared before PSS sampling")
    require(expected_root_pid in pids, "profile probe root PID is absent from its process group")
    per_pid: dict[str, int] = {}
    for pid in pids:
        per_pid[str(pid)] = read_pss_kib(pid)
    total_kib = sum(per_pid.values())
    return {
        "monotonic_ns": time.monotonic_ns(),
        "elapsed_ms": (time.monotonic_ns() - started_ns) / 1_000_000.0,
        "pids": pids,
        "per_pid_pss_kib": per_pid,
        "aggregate_pss_mb": total_kib * 1024.0 / 1_000_000.0,
    }


def run_probe_with_pss(
    probe: Path,
    profile: DeviceClass,
    titan: Path,
    work_dir: Path,
) -> tuple[subprocess.CompletedProcess[str], list[dict[str, object]], list[int]]:
    require(sys.platform.startswith("linux"), "profile certification PSS authority currently requires Linux procfs")
    require(Path("/proc/self/smaps_rollup").is_file(), "Linux smaps_rollup PSS authority is unavailable")
    command = [str(probe), profile.value, str(titan), str(work_dir)]
    process = subprocess.Popen(
        command,
        text=True,
        encoding="utf-8",
        errors="strict",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    started_ns = time.monotonic_ns()
    pgid = os.getpgid(process.pid)
    require(pgid == process.pid, "profile probe did not receive a dedicated process group")
    samples: list[dict[str, object]] = []
    observed_pids: set[int] = set()
    try:
        while True:
            if process.poll() is not None:
                break
            sample = sample_process_group(pgid, process.pid, started_ns)
            samples.append(sample)
            observed_pids.update(int(pid) for pid in sample["pids"])
            time.sleep(SAMPLE_INTERVAL_SECONDS)
        stdout, stderr = process.communicate(timeout=30.0)
    except Exception:
        try:
            os.killpg(pgid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError, OSError):
            pass
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            pass
        raise
    completed = subprocess.CompletedProcess(command, int(process.returncode or 0), stdout, stderr)
    require(samples, "profile probe produced no aggregate PSS samples")
    require(observed_pids == {process.pid}, "profile probe unexpectedly created additional process-group members")
    return completed, samples, sorted(observed_pids)


def finite_number(value: object, label: str) -> float:
    require(type(value) in {int, float}, f"{label} must be numeric")
    output = float(value)
    require(math.isfinite(output) and output >= 0.0, f"{label} must be finite and non-negative")
    return output


def integer(value: object, label: str) -> int:
    require(type(value) is int and value >= 0, f"{label} must be a non-negative integer")
    return value


def validate_probe_document(
    probe_document: Mapping[str, Any],
    profile: DeviceClass,
    titan_report: Mapping[str, Any],
) -> dict[str, Any]:
    require(probe_document.get("schema") == PROBE_SCHEMA, "profile probe schema mismatch")
    require(probe_document.get("device_class") == profile.value, "profile probe device class mismatch")
    policy = probe_document.get("runtime_policy")
    require(isinstance(policy, dict), "profile probe runtime policy is missing")
    require(policy.get("profile") == profile.value, "profile probe runtime policy class mismatch")
    require(policy.get("within_profile_budgets") is True, "profile probe exceeded runtime cache budgets")

    store = probe_document.get("store")
    require(isinstance(store, dict), "profile probe store receipt is missing")
    envelope = titan_report.get("observed_envelope")
    generation = titan_report.get("generation")
    require(isinstance(envelope, dict) and isinstance(generation, dict), "Titan report envelope/generation is missing")
    for key in (
        "logical_utf8_bytes",
        "logical_records",
        "logical_nodes",
        "style_runs",
        "resource_references",
        "largest_record_bytes",
    ):
        require(integer(store.get(key), f"probe store.{key}") == integer(envelope.get(key), f"Titan envelope.{key}"), f"profile probe store {key} differs from Titan")
    require(store.get("payload_sha256") == generation.get("payload_sha256"), "profile probe payload SHA differs from Titan")

    streaming = probe_document.get("streaming")
    preindexed = probe_document.get("preindexed")
    scroll = probe_document.get("scroll")
    search = probe_document.get("search")
    mutation = probe_document.get("mutation")
    copy = probe_document.get("copy")
    for value, label in (
        (streaming, "streaming"),
        (preindexed, "preindexed"),
        (scroll, "scroll"),
        (search, "search"),
        (mutation, "mutation"),
        (copy, "copy"),
    ):
        require(isinstance(value, dict), f"profile probe {label} receipt is missing")

    require(integer(streaming.get("preview_records"), "streaming.preview_records") > 0, "streaming preview is empty")
    require(integer(streaming.get("remaining_records"), "streaming.remaining_records") > 0, "streaming preview occurred after full import")
    require(integer(streaming.get("fragment_count"), "streaming.fragment_count") > 0, "streaming viewport has no fragments")
    require(streaming.get("truncated") is False, "streaming viewport is truncated")
    finite_number(streaming.get("milliseconds"), "streaming.milliseconds")

    require(integer(preindexed.get("fragment_count"), "preindexed.fragment_count") > 0, "preindexed viewport has no fragments")
    require(preindexed.get("truncated") is False, "preindexed viewport is truncated")
    finite_number(preindexed.get("milliseconds"), "preindexed.milliseconds")

    scroll_samples = scroll.get("samples_ms")
    scroll_coordinates = scroll.get("coordinates_q8")
    require(scroll.get("warmup_count") == 16 and scroll.get("measured_count") == 257, "scroll sample contract drifted")
    require(isinstance(scroll_samples, list) and len(scroll_samples) == 257, "scroll sample array must contain 257 values")
    require(isinstance(scroll_coordinates, list) and len(scroll_coordinates) == 257, "scroll coordinate array must contain 257 values")
    for value in scroll_samples:
        finite_number(value, "scroll sample")
    for value in scroll_coordinates:
        integer(value, "scroll coordinate")

    require(search.get("query") == "ZEVRYON_M8_TITAN_TAIL", "exact-search query drifted")
    finite_number(search.get("cold_ms"), "search.cold_ms")
    finite_number(search.get("warm_ms"), "search.warm_ms")
    records = integer(envelope.get("logical_records"), "Titan logical_records")
    require(integer(search.get("terminal_record_index"), "search.terminal_record_index") + 1 == records, "tail marker record is not terminal")
    require(integer(search.get("terminal_logical_id"), "search.terminal_logical_id") + 1 == records, "tail marker logical id is not terminal")

    mutation_samples = mutation.get("samples_us")
    mutation_indices = mutation.get("indices")
    require(mutation.get("warmup_count") == 16 and mutation.get("measured_count") == 257, "mutation sample contract drifted")
    require(isinstance(mutation_samples, list) and len(mutation_samples) == 257, "mutation sample array must contain 257 values")
    require(isinstance(mutation_indices, list) and len(mutation_indices) == 257, "mutation index array must contain 257 values")
    require(mutation.get("restored") is True, "mutation phase did not restore its original state")
    for value in mutation_samples:
        finite_number(value, "mutation sample")
    for value in mutation_indices:
        integer(value, "mutation index")

    require(integer(copy.get("source_bytes"), "copy.source_bytes") == integer(envelope.get("logical_utf8_bytes"), "Titan logical_utf8_bytes"), "copy source byte count differs from Titan")
    require(integer(copy.get("output_bytes"), "copy.output_bytes") == integer(envelope.get("logical_utf8_bytes"), "Titan logical_utf8_bytes"), "copy output byte count differs from Titan")
    require(copy.get("cancelled") is False, "copy unexpectedly cancelled")
    require(finite_number(copy.get("elapsed_seconds"), "copy.elapsed_seconds") > 0.0, "copy elapsed time must be positive")
    return dict(probe_document)


def hash_and_validate_utf8(path: Path) -> tuple[str, bool, int]:
    digest = hashlib.sha256()
    decoder = codecs.getincrementaldecoder("utf-8")("strict")
    total = 0
    valid = True
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(4 * 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            total += len(chunk)
            if valid:
                try:
                    decoder.decode(chunk, final=False)
                except UnicodeDecodeError:
                    valid = False
        if valid:
            try:
                decoder.decode(b"", final=True)
            except UnicodeDecodeError:
                valid = False
    return digest.hexdigest(), valid, total


def build_case_document(
    *,
    profile: DeviceClass,
    candidate_commit: str,
    candidate_tree: str,
    titan_report: Mapping[str, Any],
    titan_report_sha256: str,
    host_receipt_sha256: str,
    physical_ram_mib: int,
    pss_samples: list[dict[str, object]],
    probe_document: Mapping[str, Any],
    copy_sha256: str,
    copy_valid_utf8: bool,
) -> dict[str, object]:
    runtime = probe_document["runtime_policy"]
    streaming = probe_document["streaming"]
    preindexed = probe_document["preindexed"]
    scroll = probe_document["scroll"]
    search = probe_document["search"]
    mutation = probe_document["mutation"]
    copy = probe_document["copy"]
    generation = titan_report["generation"]
    case_identity = {
        "candidate_commit": candidate_commit,
        "candidate_tree": candidate_tree,
        "device_class": profile.value,
        "titan_report_sha256": titan_report_sha256,
        "container_sha256": generation["container_sha256"],
        "payload_sha256": generation["payload_sha256"],
        "physical_host_receipt_sha256": host_receipt_sha256,
    }
    case_id = sha256_bytes(canonical_bytes(case_identity))
    aggregate_pss = [float(sample["aggregate_pss_mb"]) for sample in pss_samples]
    return {
        "schema": CASE_SCHEMA,
        "authority": CASE_AUTHORITY,
        "case_id": case_id,
        "candidate_commit": candidate_commit,
        "candidate_tree": candidate_tree,
        "device_class": profile.value,
        "titan": {
            "report_sha256": titan_report_sha256,
            "container_sha256": generation["container_sha256"],
            "payload_sha256": generation["payload_sha256"],
        },
        "physical_host": {
            "authority": PHYSICAL_HOST_AUTHORITY,
            "receipt_sha256": host_receipt_sha256,
            "qualified": True,
            "device_class": profile.value,
            "physical_memory_mib": physical_ram_mib,
            "process_group_complete": True,
            "pss_authority": "aggregate-pss",
        },
        "runtime_policy": {
            "profile": runtime["profile"],
            "within_profile_budgets": runtime["within_profile_budgets"],
            "hot_budget_bytes": runtime["hot_budget_bytes"],
            "hot_allocated_bytes": runtime["hot_allocated_bytes"],
            "warm_budget_bytes": runtime["warm_budget_bytes"],
            "warm_allocated_bytes": runtime["warm_allocated_bytes"],
            "cold_budget_bytes": runtime["cold_budget_bytes"],
            "cold_allocated_bytes": runtime["cold_allocated_bytes"],
        },
        "raw": {
            "pss_samples_mb": aggregate_pss,
            "first_viewport_streaming_ms": streaming["milliseconds"],
            "first_viewport_preindexed_ms": preindexed["milliseconds"],
            "scroll_samples_ms": scroll["samples_ms"],
            "exact_search_cold_ms": search["cold_ms"],
            "exact_search_warm_ms": search["warm_ms"],
            "mutation_samples_us": mutation["samples_us"],
            "copy": {
                "source_bytes": copy["source_bytes"],
                "output_bytes": copy["output_bytes"],
                "elapsed_seconds": copy["elapsed_seconds"],
                "output_sha256": copy_sha256,
                "output_valid_utf8": copy_valid_utf8,
                "cancelled": copy["cancelled"],
            },
            "correctness": {
                "data_loss_events": 0 if copy_sha256 == generation["payload_sha256"] else 1,
                "invalid_utf8_events": 0 if copy_valid_utf8 else 1,
                "crashes_or_ooms": 0,
            },
            "probe_return_code": 0,
            "probe_terminated_abnormally": False,
        },
    }


def write_failure(attempt_dir: Path, message: str, details: Mapping[str, object] | None = None) -> None:
    failure = {
        "schema": FAILURE_SCHEMA,
        "authority": PROVENANCE_AUTHORITY,
        "error": message,
        "details": dict(details or {}),
    }
    path = attempt_dir / "failure.json"
    if not path.exists():
        try:
            write_exclusive_json(path, failure)
        except Exception:
            pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Collect one create-only physical M8 profile case with aggregate Linux process-group PSS evidence."
    )
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--profile", choices=[item.value for item in DeviceClass], required=True)
    parser.add_argument("--titan", type=Path, required=True)
    parser.add_argument("--titan-report", type=Path, required=True)
    parser.add_argument("--attempt-dir", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    attempt_dir = args.attempt_dir.resolve()
    try:
        ensure_fresh_attempt_dir(attempt_dir)
        validate_no_physical_identity_overrides()
        profile = DeviceClass(args.profile)
        probe = args.probe.resolve()
        titan_path = args.titan.resolve()
        titan_report_path = args.titan_report.resolve()
        require(probe.is_file(), f"profile probe not found: {probe}")
        require(titan_path.is_file(), f"Titan corpus not found: {titan_path}")
        require(titan_report_path.is_file(), f"Titan report not found: {titan_report_path}")

        candidate_commit, candidate_tree = clean_git_identity()
        titan_report, titan_report_sha256 = load_certification_titan(titan_report_path, titan_path)
        require(titan_report.get("candidate_commit") == candidate_commit, "Titan candidate commit differs from collector candidate")
        require(titan_report.get("candidate_tree") == candidate_tree, "Titan candidate tree differs from collector candidate")
        probe_sha_before = sha256_file(probe)

        host_before, host_before_receipt, fingerprint_before = certified_host_snapshot(
            f"m8-{profile.value}-before", profile
        )
        probe_work = attempt_dir / "probe-work"
        completed, pss_samples, observed_pids = run_probe_with_pss(
            probe, profile, titan_path, probe_work
        )
        host_after, host_after_receipt, fingerprint_after = certified_host_snapshot(
            f"m8-{profile.value}-after", profile
        )
        require(fingerprint_before == fingerprint_after, "physical host fingerprint changed during profile collection")
        probe_sha_after = sha256_file(probe)
        require(probe_sha_after == probe_sha_before, "profile probe binary changed during collection")
        require(sha256_file(titan_path) == titan_report["generation"]["container_sha256"], "Titan corpus changed during profile collection")

        host_receipt = {
            "schema": HOST_SCHEMA,
            "authority": PHYSICAL_HOST_AUTHORITY,
            "device_class": profile.value,
            "system_fingerprint": fingerprint_before,
            "before": {"host": host_before, "certification": host_before_receipt},
            "after": {"host": host_after, "certification": host_after_receipt},
            "same_system_fingerprint": True,
            "physical_host_gate_passed": True,
        }
        host_receipt_path = attempt_dir / "physical-host.json"
        host_receipt_sha256 = write_exclusive_json(host_receipt_path, host_receipt)

        if completed.returncode != 0:
            write_failure(
                attempt_dir,
                "profile probe returned a non-zero terminal status",
                {
                    "return_code": completed.returncode,
                    "stderr": completed.stderr[-8192:],
                    "pss_samples": len(pss_samples),
                },
            )
            print(f"M8 profile case failed: probe return code {completed.returncode}", file=sys.stderr)
            return 2

        probe_document = strict_json_text(completed.stdout, "profile probe stdout")
        probe_document = validate_probe_document(probe_document, profile, titan_report)
        copy_output = probe_work / "copy-output.txt"
        require(copy_output.is_file(), "profile probe copy output is missing")
        copy_sha256, copy_valid_utf8, copy_bytes = hash_and_validate_utf8(copy_output)
        require(copy_bytes == titan_report["observed_envelope"]["logical_utf8_bytes"], "copy output physical byte count differs from Titan logical payload")

        physical_ram = host_before.get("physical_ram_mib")
        require(type(physical_ram) is int, "physical RAM receipt is not an integer")
        case_document = build_case_document(
            profile=profile,
            candidate_commit=candidate_commit,
            candidate_tree=candidate_tree,
            titan_report=titan_report,
            titan_report_sha256=titan_report_sha256,
            host_receipt_sha256=host_receipt_sha256,
            physical_ram_mib=physical_ram,
            pss_samples=pss_samples,
            probe_document=probe_document,
            copy_sha256=copy_sha256,
            copy_valid_utf8=copy_valid_utf8,
        )
        case_path = attempt_dir / "profile-case.json"
        case_sha256 = write_exclusive_json(case_path, case_document)

        provenance = {
            "schema": PROVENANCE_SCHEMA,
            "authority": PROVENANCE_AUTHORITY,
            "case_id": case_document["case_id"],
            "candidate_commit": candidate_commit,
            "candidate_tree": candidate_tree,
            "device_class": profile.value,
            "probe": {
                "path": str(probe),
                "sha256": probe_sha_before,
                "return_code": completed.returncode,
                "stderr": completed.stderr,
                "stdout_sha256": sha256_bytes(completed.stdout.encode("utf-8")),
            },
            "titan": {
                "report_path": str(titan_report_path),
                "report_sha256": titan_report_sha256,
                "corpus_path": str(titan_path),
                "container_sha256": titan_report["generation"]["container_sha256"],
                "payload_sha256": titan_report["generation"]["payload_sha256"],
            },
            "physical_host": {
                "path": str(host_receipt_path),
                "sha256": host_receipt_sha256,
                "system_fingerprint": fingerprint_before,
            },
            "process_group_pss": {
                "authority": PSS_AUTHORITY,
                "sampling_interval_seconds": SAMPLE_INTERVAL_SECONDS,
                "root_pid": observed_pids[0],
                "observed_pids": observed_pids,
                "expected_single_process": True,
                "process_group_complete": True,
                "samples": pss_samples,
            },
            "phase_receipts": probe_document,
            "copy_output": {
                "path": str(copy_output),
                "sha256": copy_sha256,
                "bytes": copy_bytes,
                "valid_utf8": copy_valid_utf8,
            },
            "case": {
                "path": str(case_path),
                "sha256": case_sha256,
            },
            "collector_gate_passed": True,
        }
        provenance_path = attempt_dir / "profile-case-provenance.json"
        write_exclusive_json(provenance_path, provenance)
        print(
            f"m8_profile_case_collected=true profile={profile.value} "
            f"case_sha256={case_sha256} max_pss_mb={max(case_document['raw']['pss_samples_mb']):.6f}"
        )
        return 0
    except (
        OSError,
        ValueError,
        ProfileCaseInvalid,
        PhysicalHostEvidenceInvalid,
        subprocess.SubprocessError,
    ) as exc:
        if attempt_dir.exists():
            write_failure(attempt_dir, str(exc))
        print(f"M8 profile case collection failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
