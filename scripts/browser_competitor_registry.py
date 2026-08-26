#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import asdict, dataclass
import json
from typing import Iterable, Mapping


TERMINAL_STATES = frozenset(
    {"success", "unsupported", "unavailable", "timeout", "error", "invalid"}
)


@dataclass(frozen=True)
class CompetitorSpec:
    key: str
    canonical_name: str
    adapter: str
    canonical: bool
    playwright_browser: str | None = None
    playwright_channel: str | None = None
    launch_hint: str | None = None
    identity_note: str = ""


_REGISTRY = {
    "chromium": CompetitorSpec(
        key="chromium",
        canonical_name="Playwright Chromium",
        adapter="playwright",
        canonical=False,
        playwright_browser="chromium",
        identity_note=(
            "Auxiliary Playwright Chromium baseline; never relabel as Chrome or Edge."
        ),
    ),
    "chrome": CompetitorSpec(
        key="chrome",
        canonical_name="Google Chrome",
        adapter="playwright",
        canonical=True,
        playwright_browser="chromium",
        playwright_channel="chrome",
        identity_note="Requires an installed Google Chrome channel on the host.",
    ),
    "firefox": CompetitorSpec(
        key="firefox",
        canonical_name="Mozilla Firefox",
        adapter="playwright",
        canonical=True,
        playwright_browser="firefox",
        identity_note=(
            "Playwright-compatible Firefox build identity must be recorded explicitly."
        ),
    ),
    "edge": CompetitorSpec(
        key="edge",
        canonical_name="Microsoft Edge",
        adapter="playwright",
        canonical=True,
        playwright_browser="chromium",
        playwright_channel="msedge",
        identity_note="Requires an installed Microsoft Edge channel on the host.",
    ),
    "webkit": CompetitorSpec(
        key="webkit",
        canonical_name="WebKit",
        adapter="playwright",
        canonical=True,
        playwright_browser="webkit",
        identity_note="Playwright WebKit is WebKit evidence, not branded Safari evidence.",
    ),
    "servo": CompetitorSpec(
        key="servo",
        canonical_name="Servo",
        adapter="webdriver",
        canonical=True,
        launch_hint="--webdriver=PORT",
        identity_note="Exact Servo binary version/commit must be captured.",
    ),
    "ladybird": CompetitorSpec(
        key="ladybird",
        canonical_name="Ladybird",
        adapter="ladybird-headless",
        canonical=True,
        identity_note=(
            "Only controls demonstrated equivalent to the common scenario are admissible."
        ),
    ),
}

CANONICAL_KEYS = tuple(
    key for key, spec in _REGISTRY.items() if spec.canonical
)
AUXILIARY_KEYS = tuple(
    key for key, spec in _REGISTRY.items() if not spec.canonical
)


def validate_registry() -> None:
    expected_canonical = {"chrome", "firefox", "edge", "webkit", "servo", "ladybird"}
    if set(CANONICAL_KEYS) != expected_canonical:
        raise ValueError("canonical competitor set drifted")
    if set(CANONICAL_KEYS) & set(AUXILIARY_KEYS):
        raise ValueError("competitor cannot be both canonical and auxiliary")
    if set(_REGISTRY) != set(CANONICAL_KEYS) | set(AUXILIARY_KEYS):
        raise ValueError("registry key classification is incomplete")

    for key, spec in _REGISTRY.items():
        if spec.key != key:
            raise ValueError(f"registry key mismatch: {key}")
        if spec.adapter == "playwright" and spec.playwright_browser is None:
            raise ValueError(f"Playwright competitor lacks browser type: {key}")
        if key in {"chrome", "edge"} and spec.playwright_channel is None:
            raise ValueError(f"branded Chromium competitor lacks exact channel: {key}")
        if key == "chromium" and spec.canonical:
            raise ValueError("auxiliary Chromium cannot become canonical implicitly")


def get_spec(key: str) -> CompetitorSpec:
    try:
        return _REGISTRY[key]
    except KeyError as exc:
        raise ValueError(f"unknown competitor: {key}") from exc


def resolve_requested(keys: Iterable[str]) -> list[CompetitorSpec]:
    requested = list(keys)
    if len(requested) != len(set(requested)):
        raise ValueError("duplicate competitor request")
    return [get_spec(key) for key in requested]


def validate_terminal_record(record: Mapping[str, object]) -> CompetitorSpec:
    key = str(record.get("competitor", ""))
    spec = get_spec(key)
    status = str(record.get("status", ""))
    if status not in TERMINAL_STATES:
        raise ValueError(f"result has invalid terminal state: {status}")

    canonical_name = record.get("canonical_name")
    canonical = record.get("canonical")
    adapter = record.get("adapter")
    if canonical_name != spec.canonical_name:
        raise ValueError(f"result canonical name mismatch: {key}")
    if canonical is not spec.canonical:
        raise ValueError(f"result canonical flag mismatch: {key}")
    if adapter != spec.adapter:
        raise ValueError(f"result adapter mismatch: {key}")

    runtime_identity = record.get("runtime_identity")
    reason = record.get("reason")
    if status == "success":
        if not isinstance(runtime_identity, str) or not runtime_identity.strip():
            raise ValueError("successful competitor evidence requires runtime identity")
    else:
        if not isinstance(reason, str) or not reason.strip():
            raise ValueError(f"{status} competitor evidence requires a reason")
    return spec


def terminal_record(
    spec: CompetitorSpec,
    status: str,
    *,
    reason: str | None = None,
    runtime_identity: str | None = None,
) -> dict[str, object]:
    record: dict[str, object] = {
        "competitor": spec.key,
        "canonical_name": spec.canonical_name,
        "canonical": spec.canonical,
        "adapter": spec.adapter,
        "status": status,
        "runtime_identity": runtime_identity,
        "reason": reason,
    }
    validate_terminal_record(record)
    return record


def leadership_coverage(records: Iterable[dict[str, object]]) -> dict[str, object]:
    latest: dict[str, dict[str, object]] = {}
    for record in records:
        spec = validate_terminal_record(record)
        key = spec.key
        if key in latest:
            raise ValueError(f"duplicate terminal result for competitor: {key}")
        latest[key] = record

    missing = [key for key in CANONICAL_KEYS if key not in latest]
    unsuccessful = [
        key
        for key in CANONICAL_KEYS
        if key in latest and latest[key].get("status") != "success"
    ]
    measured = [
        key
        for key in CANONICAL_KEYS
        if key in latest and latest[key].get("status") == "success"
    ]
    return {
        "canonical_requested": list(CANONICAL_KEYS),
        "canonical_measured": measured,
        "canonical_missing": missing,
        "canonical_unsuccessful": unsuccessful,
        "full_set_coverage": not missing and not unsuccessful,
        "leadership_eligible": not missing and not unsuccessful,
    }


def registry_json() -> str:
    validate_registry()
    payload = {
        "schema": "zevryon.competitor.registry.v1",
        "canonical": list(CANONICAL_KEYS),
        "auxiliary": list(AUXILIARY_KEYS),
        "terminal_states": sorted(TERMINAL_STATES),
        "competitors": {key: asdict(spec) for key, spec in _REGISTRY.items()},
    }
    return json.dumps(payload, indent=2, sort_keys=True) + "\n"


if __name__ == "__main__":
    print(registry_json(), end="")
