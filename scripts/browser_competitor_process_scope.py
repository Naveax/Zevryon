#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
import threading
import time
from typing import Any, Callable, Iterable


class ProcessScopeUnavailable(RuntimeError):
    pass


@dataclass(frozen=True, order=True)
class ProcessIdentity:
    pid: int
    create_time_ns: int

    def as_receipt(self) -> dict[str, int]:
        return {"pid": self.pid, "create_time_ns": self.create_time_ns}


IdentitySetProvider = Callable[[int], frozenset[ProcessIdentity]]
MemoryReader = Callable[[ProcessIdentity], int]


def _load_psutil() -> Any:
    try:
        import psutil  # type: ignore[import-not-found]
    except ImportError as exc:
        raise ProcessScopeUnavailable(
            "live browser process-scope measurement requires psutil"
        ) from exc
    return psutil


def _identity_for_process(process: Any, psutil_module: Any) -> ProcessIdentity | None:
    try:
        return ProcessIdentity(
            pid=int(process.pid),
            create_time_ns=int(round(float(process.create_time()) * 1_000_000_000)),
        )
    except (
        psutil_module.Error,
        ProcessLookupError,
        ValueError,
        OverflowError,
    ):
        return None


def descendant_identities(root_pid: int) -> frozenset[ProcessIdentity]:
    psutil = _load_psutil()
    try:
        root = psutil.Process(root_pid)
        descendants = root.children(recursive=True)
    except psutil.Error:
        return frozenset()

    output: set[ProcessIdentity] = set()
    for process in descendants:
        identity = _identity_for_process(process, psutil)
        if identity is not None:
            output.add(identity)
    return frozenset(output)


def process_pss_or_rss_bytes(identity: ProcessIdentity) -> int:
    psutil = _load_psutil()
    try:
        process = psutil.Process(identity.pid)
        current = _identity_for_process(process, psutil)
        if current != identity:
            return 0
    except psutil.Error:
        return 0

    try:
        with open(
            f"/proc/{identity.pid}/smaps_rollup",
            "r",
            encoding="ascii",
            errors="ignore",
        ) as handle:
            for line in handle:
                if line.startswith("Pss:"):
                    return int(line.split()[1]) * 1024
    except (FileNotFoundError, PermissionError, ProcessLookupError, ValueError, OSError):
        pass

    try:
        return int(process.memory_info().rss)
    except (psutil.Error, ProcessLookupError):
        return 0


def dynamic_scope_identities(
    root_pid: int,
    baseline_descendants: Iterable[ProcessIdentity],
    *,
    identity_provider: IdentitySetProvider = descendant_identities,
) -> frozenset[ProcessIdentity]:
    baseline = frozenset(baseline_descendants)
    return identity_provider(root_pid) - baseline


def process_set_memory_bytes(
    identities: Iterable[ProcessIdentity],
    *,
    memory_reader: MemoryReader = process_pss_or_rss_bytes,
) -> int:
    return sum(
        max(0, int(memory_reader(identity)))
        for identity in set(identities)
    )


@dataclass(frozen=True)
class ProcessScopeSnapshot:
    identities: tuple[ProcessIdentity, ...]
    resident_bytes: int

    @property
    def valid(self) -> bool:
        return bool(self.identities)

    @property
    def receipts(self) -> list[dict[str, int]]:
        return [identity.as_receipt() for identity in self.identities]


class BrowserProcessScopeMonitor:
    """Measure descendants created after a dedicated worker's driver baseline.

    The worker runs exactly one browser case. The descendant identity set captured
    immediately before browser launch excludes the Python worker and the already
    running Playwright driver. New descendants are treated as browser-scope
    processes. Process identity includes create time so PID reuse cannot make a
    stale browser process resolve to an unrelated later process.

    Pure authority tests may inject identity/memory providers and therefore do not
    require psutil. Live measurement fails closed through ProcessScopeUnavailable
    when psutil is absent instead of silently emitting fabricated zero-memory data.
    """

    def __init__(
        self,
        root_pid: int,
        baseline_descendants: Iterable[ProcessIdentity],
        *,
        interval_seconds: float = 0.01,
        identity_provider: IdentitySetProvider = descendant_identities,
        memory_reader: MemoryReader = process_pss_or_rss_bytes,
    ) -> None:
        if interval_seconds <= 0:
            raise ValueError("interval_seconds must be positive")
        self.root_pid = int(root_pid)
        self.baseline_descendants = frozenset(baseline_descendants)
        self.interval_seconds = float(interval_seconds)
        self.identity_provider = identity_provider
        self.memory_reader = memory_reader
        self.peak_bytes = 0
        self.observed_identities: set[ProcessIdentity] = set()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._started = False

    def snapshot(self) -> ProcessScopeSnapshot:
        identities = dynamic_scope_identities(
            self.root_pid,
            self.baseline_descendants,
            identity_provider=self.identity_provider,
        )
        self.observed_identities.update(identities)
        resident = process_set_memory_bytes(
            identities,
            memory_reader=self.memory_reader,
        )
        self.peak_bytes = max(self.peak_bytes, resident)
        return ProcessScopeSnapshot(tuple(sorted(identities)), resident)

    def _run(self) -> None:
        while not self._stop.is_set():
            self.snapshot()
            time.sleep(self.interval_seconds)

    def start(self) -> None:
        if self._started:
            raise RuntimeError("browser process-scope monitor already started")
        self._started = True
        self._thread.start()

    def stop(self) -> ProcessScopeSnapshot:
        if not self._stop.is_set():
            self._stop.set()
            if self._started and self._thread.is_alive():
                self._thread.join(timeout=2.0)
        return self.snapshot()

    @property
    def valid(self) -> bool:
        return bool(self.observed_identities)

    @property
    def observed_receipts(self) -> list[dict[str, int]]:
        return [
            identity.as_receipt()
            for identity in sorted(self.observed_identities)
        ]
